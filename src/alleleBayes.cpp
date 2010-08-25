// 
// *Bayes
//
// A bayesian genetic variant parser.
// 

// standard includes
//#include <cstdio>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <iterator>
#include <algorithm>
#include <cmath>
#include <time.h>
#include <float.h>

// "boost" regular expression library
#include <boost/regex.hpp>

// "boost" string manipulation
#include <boost/algorithm/string/join.hpp>
// tuple
#include <boost/tuple/tuple.hpp>
// bind
#include <boost/bind.hpp>

// private libraries
#include "BamReader.h"
#include "Fasta.h"
#include "TryCatch.h"
#include "Parameters.h"
#include "Allele.h"
#include "AlleleParser.h"
#include "Utility.h"

#include "multichoose.h"
#include "multipermute.h"

#include "Genotype.h"
#include "GenotypePriors.h"
#include "DataLikelihood.h"
#include "ResultData.h"

//#include <boost/foreach.hpp>

#include <boost/lambda/lambda.hpp>
#include <boost/lambda/bind.hpp>



using namespace std; 

using boost::tuple;
using boost::make_tuple;

// Allele object recycling:
//
// We use the Allele freelist for performance reasons.  When an Allele object
// is destroyed, it is pushed onto this freelist.  When a new Allele object is
// created, new first checks if we have a free Allele object on the freelist.
// Because we are dynamically linked, we have to declare the freelist here,
// although it exists as a static member of the Allele class.
//
AlleleFreeList Allele::_freeList;


int main (int argc, char *argv[]) {

    AlleleParser* parser = new AlleleParser(argc, argv);
    Parameters& parameters = parser->parameters;
    list<Allele*> alleles;

    ostream& out = *(parser->output);

    // this can be uncommented to force operation on a specific set of genotypes
    //vector<Allele> genotypeAlleles;
    //genotypeAlleles.push_back(genotypeAllele(ALLELE_GENOTYPE, "A", 1));
    //genotypeAlleles.push_back(genotypeAllele(ALLELE_GENOTYPE, "T", 1));
    //genotypeAlleles.push_back(genotypeAllele(ALLELE_GENOTYPE, "G", 1));
    //genotypeAlleles.push_back(genotypeAllele(ALLELE_GENOTYPE, "C", 1));
    //vector<Genotype> genotypes = allPossibleGenotypes(parameters.ploidy, genotypeAlleles);

    vector<AlleleType> allowedAlleles;
    allowedAlleles.push_back(ALLELE_REFERENCE);
    allowedAlleles.push_back(ALLELE_SNP);
    //allowedAlleles.push_back(ALLELE_INSERTION);
    //allowedAlleles.push_back(ALLELE_DELETION);

    // output VCF header
    // TODO add proper information header fields to this, at present it's just the column and sample names
    if (parameters.output == "vcf") {
        vcfHeader(out, parser->reference->filename, parser->sampleList);
    }

    // TODO
    // ... only process potential genotypes for which we have some number of observations
    // ... optionally provide a threshold of some kind to ignore low-frequency observations that are most likely errors
    
    // we use this to force the reference allele into the analysis, which
    // allows us to detect rare variants in which the reference allele is the
    // only instance in our data
    Allele* referenceAllele;

    while (parser->getNextAlleles(alleles)) {

        filterAlleles(alleles, allowedAlleles);

        // skips 0-coverage regions
        if (alleles.size() == 0)
            continue;

        // establish a set of possible alternate alleles to evaluate at this location
        // only evaluate alleles with at least one supporting read with mapping
        // quality (MQL1) and base quality (BQL1)

        vector<Allele> genotypeAlleles;

        vector<vector<Allele*> > alleleGroups = groupAlleles(alleles, &allelesEquivalent);

        map<string, vector<Allele*> > sampleGroups = groupAllelesBySample(alleles);

        for (vector<vector<Allele*> >::iterator group = alleleGroups.begin(); group != alleleGroups.end(); ++group) {
            // for each allele that we're going to evaluate, we have to have at least one supporting read with
            // map quality >= MQL1 and the specific quality of the allele has to be >= BQL1
            for (vector<Allele*>::iterator a = group->begin(); a != group->end(); ++a) {
                Allele& allele = **a;
                if (allele.mapQuality >= parameters.MQL1 && allele.currentQuality() >= parameters.BQL1) {
                    int length = (allele.type == ALLELE_REFERENCE || allele.type == ALLELE_SNP) ? 1 : allele.length;
                    genotypeAlleles.push_back(genotypeAllele(allele.type, allele.base(), length));
                    break;
                }
            }
        }

        vector<Allele> filteredGenotypeAlleles;

        // remove genotypeAlleles for which we don't have any individuals with sufficient alternate observations
        for (vector<Allele>::iterator genotypeAllele = genotypeAlleles.begin();
                genotypeAllele != genotypeAlleles.end(); ++genotypeAllele) {

            for (map<string, vector<Allele*> >::iterator sample = sampleGroups.begin();
                    sample != sampleGroups.end(); ++sample) {

                vector<Allele*>& observedAlleles = sample->second;
                int alleleCount = 0;
                for (vector<Allele*>::iterator a = observedAlleles.begin(); a != observedAlleles.end(); ++a) {
                    if (**a == *genotypeAllele)
                        ++alleleCount;
                }
                if (alleleCount >= parameters.minAltCount 
                        && (float) alleleCount / (float) observedAlleles.size() >= parameters.minAltFraction) {
                    //cerr << *genotypeAllele << " has support of " << alleleCount 
                    //    << " in individual " << sample->first << " and fraction " 
                    //    << (float) alleleCount / (float) observedAlleles.size() << endl;
                    filteredGenotypeAlleles.push_back(*genotypeAllele);
                    goto hasSufficient;
                }
            }
            hasSufficient: ;
        }

        if (filteredGenotypeAlleles.size() <= 1)  // if we have only one viable alternate, we don't have evidence for variation at this site
            continue;

        //cerr << filteredGenotypeAlleles << endl;

        vector<Genotype> genotypes = allPossibleGenotypes(parameters.ploidy, filteredGenotypeAlleles);

        // continue in the case that no individual has more than some
        // fraction of alternate allele observations ...  (default is 0.1)
        //if (!sufficientAlternateObservations(sampleGroups, parameters.minAltCount, parameters.minAltFraction))
         //   continue;

        Results results;

        // calculate data likelihoods
        for (map<string, vector<Allele*> >::iterator sample = sampleGroups.begin();
                sample != sampleGroups.end(); ++sample) {

            string sampleName = sample->first;
            vector<Allele*>& observedAlleles = sample->second;

            vector<pair<Genotype*, long double> > probs = 
                probObservedAllelesGivenGenotypes(observedAlleles, genotypes);

            /*
            vector<pair<Genotype*, long double> > approxprobs = 
                approxProbObservedAllelesGivenGenotypes(observedAlleles, genotypes);
            vector<pair<Genotype*, long double> > bambayesapproxprobs = 
                bamBayesApproxProbObservedAllelesGivenGenotypes(observedAlleles, genotypes);
            //vector<pair<Genotype*, long double> > exactprobs = 
                //exactProbObservedAllelesGivenGenotypes(observedAlleles, genotypes);

            cout << sampleName << endl;
            for (vector<Allele*>::iterator i = observedAlleles.begin(); i != observedAlleles.end(); ++i) {
                Allele& allele = **i;
                cout << allele.base() << " ";
            }
            cout << endl;
            vector<pair<Genotype*, long double> >::iterator d = approxprobs.begin();
            vector<pair<Genotype*, long double> >::iterator b = bambayesapproxprobs.begin();
            //vector<pair<Genotype*, long double> >::iterator e = exactprobs.begin();
            for ( ; d != approxprobs.end() && b != bambayesapproxprobs.end(); ++d, ++b) {
                //cout << "exact  : " << *e->first << " " << e->second << endl;
                cout << "bapprox: " << *b->first << " " << b->second << endl;
                cout << "dapprox: " << *d->first << " " << d->second << endl;
                cout << endl;
            }
            */
            
            map<Genotype*, long double> marginals;
            map<Genotype*, vector<long double> > rawMarginals;

            results.insert(make_pair(sampleName, ResultData(sampleName, probs, marginals, rawMarginals, observedAlleles)));

        }

        // sort genotype data likelihoods and accumulate into 
        
        vector<pair<string, vector<pair<Genotype*, long double> > > > sampleGenotypes;
        for (Results::iterator s = results.begin(); s != results.end(); ++s) {
            s->second.sortDataLikelihoods();
            /*
            for (vector<pair<Genotype, long double> >::iterator p = s->second.dataLikelihoods.begin(); p != s->second.dataLikelihoods.end(); ++p) {
                cout << p->first << " " << p->second << endl;
            }
            cout << endl;
            */

            sampleGenotypes.push_back(make_pair(s->first, s->second.dataLikelihoods));
        }


        // calculate genotype combo likelihoods, integral over nearby genotypes
        // calculate marginals
        // and determine best genotype combination

        // TODO add in all-alternate and all-reference
        // ... or at least very add one which is consistent
        //vector<GenotypeCombo> bandedCombos = bandedGenotypeCombinations(sampleGenotypes, 2, 2);
        //vector<GenotypeCombo> bandedCombos = bandedGenotypeCombinationsIncludingBestHomozygousCombo(sampleGenotypes, 2, 2);
        vector<GenotypeCombo> bandedCombos = bandedGenotypeCombinationsIncludingAllHomozygousCombos(sampleGenotypes, genotypes, 2, 2);

        vector<pair<GenotypeCombo, long double> > genotypeComboProbs;

        for (vector<GenotypeCombo>::iterator combo = bandedCombos.begin(); combo != bandedCombos.end(); ++combo) {

            long double probabilityObservationsGivenGenotypes = 0;
            vector<Genotype*> genotypeCombo;

            for (GenotypeCombo::iterator i = combo->begin(); i != combo->end(); ++i) {
                genotypeCombo.push_back(i->second.first);
                probabilityObservationsGivenGenotypes += i->second.second;
            }

            /*
            map<Allele, int> alleleCounts = countAlleles(genotypeCombo);
            cout << "alleleCounts = ";
            for (map<Allele, int>::iterator a = alleleCounts.begin(); a != alleleCounts.end(); ++a) {
                cout << " ( " << a->first.base() << " " << a->second << ")";
            }
            cout << endl;

            map<int, int> frequencyCounts = countFrequencies(genotypeCombo);
            */

            long double priorProbabilityOfGenotypeCombo = alleleFrequencyProbabilityln(countFrequencies(genotypeCombo), parameters.TH);
            //cout << "priorProbabilityOfGenotypeCombo = " << priorProbabilityOfGenotypeCombo << endl;
            //cout << "probabilityObservationsGivenGenotypes = " << probabilityObservationsGivenGenotypes << endl;
            long double comboProb = priorProbabilityOfGenotypeCombo + probabilityObservationsGivenGenotypes;

            // XXX hack to prevent underflow
            //if (comboProb < DBL_MIN_EXP)
                //continue;

            for (GenotypeCombo::iterator i = combo->begin(); i != combo->end(); ++i) {
                map<Genotype*, vector<long double> >& marginals = results[i->first].rawMarginals;
                Genotype* genotype = i->second.first;
                //long double& prob = i->second.second;
                map<Genotype*, vector<long double> >::iterator marginal = marginals.find(genotype);
                if (marginal == marginals.end()) {
                    vector<long double> probs;
                    probs.push_back(comboProb);
                    marginals.insert(make_pair(genotype, probs));
                } else {
                    marginals[genotype].push_back(comboProb);
                }
            }

            genotypeComboProbs.push_back(make_pair(*combo, comboProb));

        }

        // XXX AWFUL hack, but necessary to guard against the case that all our genotype probabilites are <-200 log
        //if (genotypeComboProbs.size() == 0)
            //continue;

        // genotype_combo_probs = sorted(genotype_combo_probs, key=lambda c: c[1], reverse=True)
        sort(genotypeComboProbs.begin(), genotypeComboProbs.end(),
                boost::bind(&pair<GenotypeCombo, long double>::second, _1) 
                    > boost::bind(&pair<GenotypeCombo, long double>::second, _2));

        vector<long double> comboProbs;
        comboProbs.resize(genotypeComboProbs.size());
        transform(genotypeComboProbs.begin(), genotypeComboProbs.end(),
                comboProbs.begin(), boost::bind(&pair<GenotypeCombo, long double>::second, _1));

        /*
        for (vector<pair<GenotypeCombo, long double> >::iterator c = genotypeComboProbs.begin(); c != genotypeComboProbs.end(); ++c) {
            cout << "prob:" << c->second << endl;
        }
        */
        long double posteriorNormalizer = logsumexp_probs(comboProbs);
        //cout << "posteriorNormalizer = " << posteriorNormalizer << endl;
        /*
        cout << "comboProbs = [";
        for (vector<long double>::iterator i = comboProbs.begin(); i != comboProbs.end(); ++i)
            cout << *i << " ";
        cout << "]" << endl;
        */
        

        // TODO Gabor had a suggestion about this normalization, that we only normalize 'perturbed' cases
        // ....  but i think that this thought was not asserted as necessary
        // normalize marginals
        for (Results::iterator r = results.begin(); r != results.end(); ++r) {
            ResultData& d = r->second;
            //cout << r->first << endl;
            for (map<Genotype*, vector<long double> >::iterator m = d.rawMarginals.begin(); m != d.rawMarginals.end(); ++m) {
                /*
                cout << *m->first << " rawMarginals = [";
                for (vector<long double>::iterator i = m->second.begin(); i != m->second.end(); ++i)
                    cout << *i << ", ";
                cout << "]" << endl;
                cout << logsumexp_probs(m->second) << endl;
                */
                d.marginals[m->first] = logsumexp_probs(m->second) - posteriorNormalizer;
            }
        }

        // we provide p(var|data), or the probability that the location has
        // variation between individuals relative to the probability that it
        // has no variation
        //
        // in other words:
        // p(var|d) = 1 - p(AA|d) - p(TT|d) - P(GG|d) - P(CC|d)
        //
        // the approach is go through all the homozygous combos
        // and then subtract this from 1... resolving p(var|d)

        long double pVar = 1.0;

        for (vector<pair<GenotypeCombo, long double> >::iterator gc = genotypeComboProbs.begin(); gc != genotypeComboProbs.end(); ++gc) {
            /*
            for (GenotypeCombo::iterator c = gc->first.begin(); c != gc->first.end(); ++c)
                cout << *c->second.first << " ";
            cout << endl;
            cout << (long double) gc->second << endl;
            */
            if (isHomozygousCombo(gc->first)) {
                pVar -= safe_exp(gc->second - posteriorNormalizer);
            }
        }

        // this is okay... as we use this to pick our best genotypes
        GenotypeCombo& bestGenotypeCombo = genotypeComboProbs.front().first;
        long double bestGenotypeComboProb = safe_exp(genotypeComboProbs.front().second - posteriorNormalizer);
        //cout << bestGenotypeComboProb << endl;
        vector<Genotype*> bestComboGenotypes;
        for (GenotypeCombo::iterator g = bestGenotypeCombo.begin(); g != bestGenotypeCombo.end(); ++g)
            bestComboGenotypes.push_back(g->second.first);
        long double bestAlleleSamplingProb = safe_exp(alleleFrequencyProbabilityln(countFrequencies(bestComboGenotypes), parameters.TH));

        if (!parameters.suppressOutput) {
            //cerr << parser->currentPosition << " " << alleles.size() << " " << bestGenotypeComboProb << " " << genotypeComboProbs.front().second << " " <<  posteriorNormalizer << endl;

            if (parameters.output == "json") {
                out << "{ \"position\": " << parser->currentPosition + 1 // 1-based reporting, to match vcf
                    << ", \"sequence\": " << parser->currentTarget->seq
                    << ", \"best_genotype_combo\":" << bestGenotypeCombo
                    << ", \"combos_tested\":" << bandedCombos.size()
                    << ", \"best_genotype_combo_prob\":" << bestGenotypeComboProb 
                    << ", \"coverage\":" << alleles.size()
                    << ", \"posterior_normalizer\":" << safe_exp(posteriorNormalizer)
                    << ", \"ewens_sampling_probability\":" << bestAlleleSamplingProb
                    << ", \"samples\":";
                json(out, results, parser);
                out << "}" << endl;

            }
            if (pVar >= parameters.PVL) {
                if (parameters.output == "vcf") {
                    bool hasVariant = false;
                    string alternateBase;
                    string referenceBase = parser->currentReferenceBase();
                    // get the set of unique alternate alleles at this site
                    map<Allele, bool> alternates;
                    for (Results::iterator r = results.begin(); r != results.end(); ++r) {
                        ResultData& sample = r->second; 
                        Genotype* g = sample.bestMarginalGenotype().first;
                        vector<Allele> alts = g->alternateAlleles(referenceBase);
                        for (vector<Allele>::iterator a = alts.begin(); a != alts.end(); ++a)
                            alternates[*a] = true;
                    }
                    // for each unique alternate allele, output a line of vcf
                    for (map<Allele, bool>::iterator alt = alternates.begin(); alt != alternates.end(); ++alt) {
                        out << vcf(pVar,
                                bestAlleleSamplingProb,
                                alt->first.base(),
                                parser->sampleList,
                                alleles,
                                results,
                                parser)
                            << endl;
                    }
                }
            }
            
        }

    }

    delete parser;

    return 0;

}
