#include "Estimate.h"
#include "Histogram.h"
#include "IOUtil.h"
#include "MLE.h"
#include "PDF.h"
#include "SAM.h"
#include "Uncompress.h"
#include <algorithm>
#include <cassert>
#include <climits>
#include <cstdlib>
#include <fstream>
#include <getopt.h>
#include <iomanip>
#include <iostream>
#include <limits> // for numeric_limits
#include <sstream>
#include <string>
#include <vector>
#if _OPENMP
# include <omp.h>
#endif

using namespace std;

#define PROGRAM "DistanceEst"

static const char VERSION_MESSAGE[] =
PROGRAM " (" PACKAGE_NAME ") " VERSION "\n"
"Written by Jared Simpson and Shaun Jackman.\n"
"\n"
"Copyright 2011 Canada's Michael Smith Genome Science Centre\n";

static const char USAGE_MESSAGE[] =
"Usage: " PROGRAM " [OPTION]... HIST [PAIR]\n"
"Estimate distances between contigs using paired-end alignments.\n"
"  HIST  distribution of fragments size\n"
"  PAIR  alignments between contigs\n"
"\n"
"  -k, --kmer=KMER_SIZE  k-mer size\n"
"  -n, --npairs=NPAIRS   minimum number of pairs\n"
"  -s, --seed-length=L   minimum length of the seed contigs\n"
"  -q, --min-mapq=N      ignore alignments with mapping quality\n"
"                        less than this threshold [1]\n"
"  -o, --out=FILE        write result to FILE\n"
"      --dot             output overlaps in dot format\n"
"  -j, --threads=N       use N parallel threads [1]\n"
"  -v, --verbose         display verbose output\n"
"      --help            display this help and exit\n"
"      --version         output version information and exit\n"
"\n"
"Report bugs to <" PACKAGE_BUGREPORT ">.\n";

namespace opt {
	unsigned k; // used by MLE

	/** Output in dot format. */
	static int dot;
	int format = ADJ; // used by Estimate

	static unsigned seedLen;
	static unsigned npairs;
	static unsigned minMapQ = 1;

	/** Reverse-forward mate pair orientation. */
	static bool rf = false;

	static int verbose;
	static string out;
	static int threads = 1;
}

static const char shortopts[] = "j:k:n:o:q:s:v";

enum { OPT_HELP = 1, OPT_VERSION };

static const struct option longopts[] = {
	{ "dot",         no_argument,       &opt::dot, 1, },
	{ "kmer",        required_argument, NULL, 'k' },
	{ "npairs",      required_argument, NULL, 'n' },
	{ "out",         required_argument, NULL, 'o' },
	{ "min-mapq",    required_argument, NULL, 'q' },
	{ "seed-length", required_argument, NULL, 's' },
	{ "threads",     required_argument,	NULL, 'j' },
	{ "verbose",     no_argument,       NULL, 'v' },
	{ "help",        no_argument,       NULL, OPT_HELP },
	{ "version",     no_argument,       NULL, OPT_VERSION },
	{ NULL, 0, NULL, 0 }
};

typedef vector<SAMRecord> AlignPairVec;

/** Estimate the distance between two contigs.
 * @param numPairs [out] the number of pairs that agree with the
 * expected distribution
 * @return the estimated distance
 */
static int estimateDistance(unsigned len0, unsigned len1,
		const AlignPairVec& pairs, const PDF& pdf,
		unsigned& numPairs)
{
	// The provisional fragment sizes are calculated as if the contigs
	// were perfectly adjacent with no overlap or gap.
	typedef vector<pair<int, int> > Fragments;
	Fragments fragments;
	fragments.reserve(pairs.size());
	for (AlignPairVec::const_iterator it = pairs.begin();
			it != pairs.end(); ++it) {
		int a0 = it->targetAtQueryStart();
		int a1 = it->mateTargetAtQueryStart();
		if (it->isReverse())
			a0 = len0 - a0;
		if (!it->isMateReverse())
			a1 = len1 - a1;
		fragments.push_back(opt::rf
				? make_pair(a1, len1 + a0)
				: make_pair(a0, len0 + a1));
	}

	// Remove duplicate fragments.
	sort(fragments.begin(), fragments.end());
	fragments.erase(unique(fragments.begin(), fragments.end()),
			fragments.end());
	numPairs = fragments.size();
	if (numPairs < opt::npairs)
		return INT_MIN;

	vector<int> fragmentSizes;
	fragmentSizes.reserve(fragments.size());
	for (Fragments::const_iterator it = fragments.begin();
			it != fragments.end(); ++it)
		fragmentSizes.push_back(it->second - it->first);

	return maximumLikelihoodEstimate(-opt::k+1, pdf.getMaxIdx(),
			fragmentSizes, pdf, len0, len1, numPairs);
}

static void writeEstimate(ostream& out,
		const ContigNode& id0, const ContigNode& id1,
		unsigned len0, unsigned len1,
		const AlignPairVec& pairs, const PDF& pdf)
{
	if (pairs.size() < opt::npairs)
		return;

	Estimate est;
	est.contig = id1;
	est.distance = estimateDistance(len0, len1,
			pairs, pdf, est.numPairs);
	est.stdDev = pdf.getSampleStdDev(est.numPairs);

	if (est.numPairs >= opt::npairs) {
		if (opt::dot) {
			if (id0.sense())
				est.contig.flip();
#pragma omp critical(out)
			out << '"' << id0 << "\" -> " << est << '\n';
		} else
			out << ' ' << est;
	} else if (opt::verbose > 1) {
#pragma omp critical(cerr)
		cerr << "warning: " << id0 << ',' << id1 << ' '
			<< est.numPairs << " of " << pairs.size()
			<< " pairs fit the expected distribution\n";
	}
}

/** Generate distance estimates for the specified alignments. */
static void writeEstimates(ostream& out,
		const vector<SAMRecord>& pairs,
		const vector<unsigned>& lengthVec, const PDF& pdf)
{
	assert(!pairs.empty());
	ContigID id0(pairs.front().rname);
	assert(id0 < lengthVec.size());
	unsigned len0 = lengthVec[id0];
	if (len0 < opt::seedLen)
		return; // Skip contigs shorter than the seed length.

	ostringstream ss;
	if (!opt::dot)
		ss << pairs.front().rname;

	typedef map<ContigNode, AlignPairVec> Pairs;
	Pairs dataMap[2];
	for (AlignPairVec::const_iterator it = pairs.begin();
			it != pairs.end(); ++it)
		dataMap[it->isReverse()][ContigNode(it->mrnm,
				it->isReverse() == it->isMateReverse())]
			.push_back(*it);

	for (int sense0 = false; sense0 <= true; sense0++) {
		if (!opt::dot && sense0)
			ss << " ;";
		const Pairs& x = dataMap[sense0 ^ opt::rf];
		for (Pairs::const_iterator it = x.begin();
				it != x.end(); ++it)
			writeEstimate(opt::dot ? out : ss,
					ContigNode(id0, sense0), it->first,
					len0, lengthVec[it->first.id()],
					it->second, pdf);
	}
	if (!opt::dot)
#pragma omp critical(out)
		out << ss.str() << '\n';
	assert(out.good());
}

/** Load a histogram from the specified file. */
static Histogram loadHist(const string& path)
{
	ifstream in(path.c_str());
	assert_good(in, path);

	Histogram hist;
	in >> hist;
	assert(in.eof());

	if (hist.empty()) {
		cerr << "error: the histogram `" << path << "' is empty\n";
		exit(EXIT_FAILURE);
	}
	return hist;
}

/** Read contig lengths from SAM headers. */
static void readContigLengths(istream& in, vector<unsigned>& lengths)
{
	assert(in);
	assert(lengths.empty());
	assert(ContigID::empty());
	for (string line; in.peek() == '@' && getline(in, line);) {
		istringstream ss(line);
		string type, tag;
		ss >> type;
		if (type != "@SQ")
			continue;
		ss >> ws;

		getline(ss, tag, ':');
		assert(tag == "SN");
		ContigID id;
		ss >> id >> ws;

		getline(ss, tag, ':');
		assert(tag == "LN");
		unsigned len;
		ss >> len;

		assert(ss);
		lengths.push_back(len);
	}
	if (lengths.empty()) {
		cerr << PROGRAM ": error: no @SQ records in the SAM header\n";
		exit(EXIT_FAILURE);
	}
}

int main(int argc, char** argv)
{
	bool die = false;
	for (int c; (c = getopt_long(argc, argv,
					shortopts, longopts, NULL)) != -1;) {
		istringstream arg(optarg != NULL ? optarg : "");
		switch (c) {
			case '?': die = true; break;
			case 'j': arg >> opt::threads; break;
			case 'k': arg >> opt::k; break;
			case 'n': arg >> opt::npairs; break;
			case 'o': arg >> opt::out; break;
			case 'q': arg >> opt::minMapQ; break;
			case 's': arg >> opt::seedLen; break;
			case 'v': opt::verbose++; break;
			case OPT_HELP:
				cout << USAGE_MESSAGE;
				exit(EXIT_SUCCESS);
			case OPT_VERSION:
				cout << VERSION_MESSAGE;
				exit(EXIT_SUCCESS);
		}
	}

	if (opt::k <= 0) {
		cerr << PROGRAM ": missing -k,--kmer option\n";
		die = true;
	}

	if (opt::seedLen <= 0) {
		cerr << PROGRAM ": missing -s,--seed-length option\n";
		die = true;
	}

	if (opt::npairs <= 0) {
		cerr << PROGRAM ": missing -n,--npairs option\n";
		die = true;
	}

	if (argc - optind < 1) {
		cerr << PROGRAM ": missing arguments\n";
		die = true;
	} else if (argc - optind > 2) {
		cerr << PROGRAM ": too many arguments\n";
		die = true;
	}

	if (die) {
		cerr << "Try `" << PROGRAM
			<< " --help' for more information.\n";
		exit(EXIT_FAILURE);
	}

	if (opt::dot)
		opt::format = DOT;

	if (opt::seedLen < 2*opt::k)
		cerr << "warning: the seed-length should be at least twice k:"
			" k=" << opt::k << ", s=" << opt::seedLen << '\n';

#if _OPENMP
	if (opt::threads > 0)
		omp_set_num_threads(opt::threads);
#endif

	string distanceCountFile(argv[optind++]);
	string alignFile(argv[optind] == NULL ? "-" : argv[optind++]);

	ifstream inFile(alignFile.c_str());
	istream& in(strcmp(alignFile.c_str(), "-") == 0 ? cin : inFile);

	if (strcmp(alignFile.c_str(), "-") != 0)
		assert_good(inFile, alignFile);

	ofstream outFile;
	if (!opt::out.empty()) {
		outFile.open(opt::out.c_str());
		assert(outFile.is_open());
	}
	ostream& out = opt::out.empty() ? cout : outFile;

	if (opt::dot)
		out << "digraph dist {\ngraph ["
			"k=" << opt::k << " "
			"s=" << opt::seedLen << " "
			"n=" << opt::npairs << "]\n";

	// Read the contig lengths.
	vector<unsigned> contigLens;
	readContigLengths(in, contigLens);
	ContigID::lock();

	// Read the fragment size distribution.
	Histogram distanceHist = loadHist(distanceCountFile);
	unsigned numRF = distanceHist.count(INT_MIN, 0);
	unsigned numFR = distanceHist.count(1, INT_MAX);
	unsigned numTotal = distanceHist.size();
	if (opt::verbose > 0)
		cerr << "Mate orientation FR: " << numFR << setprecision(3)
			<< " (" << (float)100*numFR/numTotal << "%)"
			<< " RF: " << numRF << setprecision(3)
			<< " (" << (float)100*numRF/numTotal << "%)"
			<< endl;
	if (numFR < numRF) {
		cerr << "The mate pairs of this library are oriented "
			"reverse-forward (RF)." << endl;
		opt::rf = true;
		distanceHist = distanceHist.negate();
	}

	distanceHist.eraseNegative();
	Histogram h = distanceHist.trimFraction(0.0001);
	if (opt::verbose > 0)
		cerr << "Stats mean: " << setprecision(4) << h.mean() << " "
			"median: " << setprecision(4) << h.median() << " "
			"sd: " << setprecision(4) << h.sd() << " "
			"n: " << h.size() << " "
			"min: " << h.minimum() << " max: " << h.maximum() << '\n'
			<< h.barplot() << endl;
	PDF empiricalPDF(h);

	// Check that the input is sorted.
	vector<bool> seen(contigLens.size());

	// Estimate the distances between contigs.
	vector<SAMRecord> pairs(1);
	in >> pairs.front();
	assert(in);
#pragma omp parallel
	for (vector<SAMRecord> privatePairs;;) {
		privatePairs.clear();
#pragma omp critical(in)
		for (SAMRecord sam;;) {
			if (in >> sam == false) {
				privatePairs.swap(pairs);
				assert(pairs.empty());
				break;
			}

			if (sam.isUnmapped() || sam.isMateUnmapped()
					|| !sam.isPaired() || sam.rname == sam.mrnm
					|| sam.mapq < opt::minMapQ)
				continue;
			// Clear unused fields.
			sam.qname.clear();
#if SAM_SEQ_QUAL
			sam.seq.clear();
			sam.qual.clear();
#endif

			assert(!pairs.empty());
			if (sam.rname == pairs.front().rname) {
				pairs.push_back(sam);
			} else {
				ContigID id(sam.rname);
				if (seen[id]) {
					cerr << "error: input must be sorted: `"
						<< sam.rname << "'\n";
					exit(EXIT_FAILURE);
				}
				seen[id] = true;

				privatePairs.swap(pairs);
				assert(pairs.empty());
				pairs.push_back(sam);
				break;
			}
		}

		if (privatePairs.empty())
			break;
		writeEstimates(out, privatePairs, contigLens, empiricalPDF);
	}
	assert(in.eof());

	if (opt::dot)
		out << "}\n";
	return 0;
}
