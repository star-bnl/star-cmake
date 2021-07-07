#ifndef FWD_TRACKER_H
#define FWD_TRACKER_H

#include "Fit/Fitter.h"
#include "TFile.h"
#include "TGraph2D.h"
#include "TH1.h"
#include "TH1F.h"
#include "TH2F.h"
#include "TRandom3.h"
#include "TTree.h"
#include "TVector3.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "StFwdTrackMaker/include/Tracker/ConfigUtil.h"
#include "StFwdTrackMaker/include/Tracker/FastSim.h"
#include "StFwdTrackMaker/include/Tracker/FwdHit.h"
#include "StFwdTrackMaker/include/Tracker/HitLoader.h"
#include "StFwdTrackMaker/include/Tracker/QualityPlotter.h"
#include "StFwdTrackMaker/include/Tracker/TrackFitter.h"
#include "StFwdTrackMaker/include/Tracker/VertexFinder.h"
#include "StFwdTrackMaker/include/Tracker/loguru.h"

#include "Criteria/Criteria.h"
#include "Criteria/ICriterion.h"
#include "KiTrack/Automaton.h"
#include "KiTrack/SegmentBuilder.h"
#include "KiTrack/SubsetHopfieldNN.h"

#include "CriteriaKeeper.h"

#include "GenFit/FitStatus.h"

#include "StFwdTrackMaker/XmlConfig/XmlConfig.h"

// Utility class for evaluating ID and QA truth
struct MCTruthUtils {

    static int domCon(std::vector<KiTrack::IHit *> hits, float &qual) {
        int numberOfHits = hits.size();
        std::map<int, int> count;
        int total = 0;

        for (auto *h : hits) {
            auto *hit = dynamic_cast<FwdHit *>(h);
            if (0 == hit)
                continue;
            int idtruth = hit->_tid;
            count[idtruth]++;
            total++;
        }

        int cmx = 0; // count max
        int idtruth = 0;

        for (auto &iter : count) {
            if (iter.second > cmx) {
                cmx = iter.second;
                idtruth = iter.first;
            }
        }

        if (total > 0)
            qual = float(cmx) / float(total);

        return idtruth;
    };
};

class ForwardTrackMaker {
  public:
    ForwardTrackMaker() : tree(nullptr), fInput(nullptr), configFile("config.xml") {
        LOG_SCOPE_FUNCTION(INFO);
    }

    void setOverrides(map<string, string> cmdConfig) {
        cmdLineConfig = cmdConfig;
    }

    void setConfigFile(std::string cf) {
        configFile = cf;
    }

    void setSaveCriteriaValues(bool save) {
        saveCriteriaValues = save;
    }

    // Adopt external configuration file
    void setConfig(jdb::XmlConfig &_cfg) { cfg = _cfg; }
    // Adopt external hit loader
    void setLoader(IHitLoader *loader) { hitLoader = loader; }

    virtual void initialize() {
        setupHistograms();

        doTrackFitting = !(cfg.get<bool>("TrackFitter:off", false));
        if (cfg.exists("TrackFitter") == false)
            doTrackFitting = false;
    }

    void init() {
        LOG_SCOPE_FUNCTION(INFO);

        LOG_F(INFO, "CONFIG FILE: %s", configFile.c_str());
        cfg.loadFile(configFile, cmdLineConfig);
        string datatype = cfg.get<string>("Input:type", "sim_mc");
        LOG_F(INFO, "Data type: %s", datatype.c_str());

        if ("sim_mc" == datatype) {
            // initInputFromMC();
            hitLoader = new McHitLoader(cfg);
        } else if ("fastsim_mc" == datatype || "fastsim_real" == datatype || "fastsim_ghost" == datatype) {
            hitLoader = new FastSimHitLoader(cfg);
        }

        if (nullptr != hitLoader)
            nEvents = hitLoader->nEvents();

        // Initialize the Tracker System of measure
        gFwdSystem = new FwdSystem(7);

        gRandom = new TRandom3();
        gRandom->SetSeed(1);

        // make our quality plotter
        qPlotter = new QualityPlotter(cfg);
        qPlotter->makeHistograms(cfg.get<size_t>("TrackFinder:nIterations", 1));

        trackFitter = new TrackFitter(cfg);
        trackFitter->setup(cfg.get<bool>("TrackFitter:display"));

        setupHistograms();
    }

    void writeEventHistograms() {
        LOG_SCOPE_FUNCTION(INFO);
        // build the name
        string name = "results.root";

        if (cfg.exists("Output:url")) {
            name = cfg.get<string>("Output:url");
        }

        LOG_F(INFO, "EventSummary : %s", name.c_str());

        TFile *fOutput = new TFile(name.c_str(), "RECREATE");
        fOutput->cd();
        // write out the config we use (do before histos):
        TNamed n("cfg", cfg.toXml());
        n.Write();

        // fOutput->mkdir( "Input/" );
        // fOutput->cd("Input/");
        writeHistograms();

        fOutput->mkdir("Fit/");
        fOutput->cd("Fit/");
        trackFitter->writeHistograms();
        fOutput->cd("");
        qPlotter->writeHistograms();
    }

    /** Loads Criteria from XML configuration.
   *
   * Utility function for loading criteria from XML config.
   *
   * @return vector of ICriterion pointers
   */
    std::vector<KiTrack::ICriterion *> loadCriteria(string path) {
        std::vector<KiTrack::ICriterion *> crits;
        auto paths = cfg.childrenOf(path);

        for (string p : paths) {
            string name = cfg.get<string>(p + ":name");
            bool active = cfg.get<bool>(p + ":active", true);

            if (false == active) {
                LOG_F(INFO, "Skipping Criteria %s (active=false)", name.c_str());
                continue;
            }

            float vmin = cfg.get<float>(p + ":min", 0);
            float vmax = cfg.get<float>(p + ":max", 1);
            LOG_F(INFO, "Loading Criteria from %s (name=%s, min=%f, max=%f)", p.c_str(), name.c_str(), vmin, vmax);
            auto crit = KiTrack::Criteria::createCriterion(name, vmin, vmax);
            crit->setSaveValues(saveCriteriaValues);
            if (saveCriteriaValues)
                crits.push_back(new KiTrack::CriteriaKeeper(crit)); // KiTrack::CriteriaKeeper intercepts values and saves them
            else
                crits.push_back(crit);
        }

        return crits;
    }

    std::vector<float> getCriteriaValues(std::string crit_name) {
        std::vector<float> em;
        if (saveCriteriaValues != true) {
            return em;
        }

        for (auto crit : twoHitCrit) {
            if (crit_name == crit->getName()) {
                auto critKeeper = static_cast<KiTrack::CriteriaKeeper *>(crit);
                return critKeeper->getValues();
            }
        }

        for (auto crit : threeHitCrit) {
            if (crit_name == crit->getName()) {
                auto critKeeper = static_cast<KiTrack::CriteriaKeeper *>(crit);
                return critKeeper->getValues();
            }
        }

        return em;
    };

    std::vector<int> getCriteriaTrackIds(std::string crit_name) {
        std::vector<int> em;
        if (saveCriteriaValues != true) {
            return em;
        }

        for (auto crit : twoHitCrit) {
            if (crit_name == crit->getName()) {
                auto critKeeper = static_cast<KiTrack::CriteriaKeeper *>(crit);
                return critKeeper->getTrackIds();
            }
        }

        for (auto crit : threeHitCrit) {
            if (crit_name == crit->getName()) {
                auto critKeeper = static_cast<KiTrack::CriteriaKeeper *>(crit);
                return critKeeper->getTrackIds();
            }
        }

        return em;
    };

    size_t nHitsInHitMap(std::map<int, std::vector<KiTrack::IHit *>> &hitmap) {
        size_t n = 0;

        for (auto kv : hitmap) {
            n += kv.second.size();
        }

        return n;
    }

    size_t countRecoTracks(size_t nHits) {
        size_t n = 0;

        for (auto t : recoTracks) {
            if (t.size() == nHits)
                n++;
        }

        return n;
    }

    void setupHistograms() {
        LOG_SCOPE_FUNCTION(INFO);

        hist["input_nhits"] = new TH1I("input_nhits", ";# hits", 1000, 0, 1000);
        hist["nAttemptedFits"] = new TH1I("nAttemptedFits", ";;# attempted fits", 10, 0, 10);
        hist["nPossibleFits"] = new TH1I("nPossibleFits", ";;# possible fits", 10, 0, 10);
        // refit with silicon
        hist["nPossibleReFits"] = new TH1I("nPossibleReFits", ";;# possible REfits", 10, 0, 10);
        hist["nAttemptedReFits"] = new TH1I("nAttemptedReFits", ";;#attempted REfits", 10, 0, 10);
        hist["nFailedReFits"] = new TH1I("nFailedReFits", ";;# failed REfits", 10, 0, 10);

        hist["FitStatus"] = new TH1I("FitStatus", ";;# failed REfits", 15, 0, 15);
        jdb::HistoBins::labelAxis(hist["FitStatus"]->GetXaxis(), {"Seeds", "AttemptFit", "GoodFit", "BadFit", "GoodCardinal", "PossibleReFit", "AttemptReFit", "GoodReFit", "BadReFit", "w3Si","w2Si", "w1Si", "w0Si" });

        hist["FitDuration"] = new TH1I("FitDuration", ";Duration (ms)", 5000, 0, 50000);
        hist["nSiHitsFound"] = new TH2I( "nSiHitsFound", ";Si Disk; n Hits", 5, 0, 5, 10, 0, 10 );
    }

    void fillHistograms() {
        LOG_SCOPE_FUNCTION(INFO);

        if (hitLoader != nullptr) {
            LOG_F(INFO, "h=%p", hist["input_nhits"]);
            auto hm = hitLoader->load(1);
            for (auto hp : hm)
                hist["input_nhits"]->Fill(hp.second.size());
        }
    }

    void writeHistograms() {
        LOG_SCOPE_FUNCTION(INFO);
        for (auto nh : hist) {
            nh.second->SetDirectory(gDirectory);
            nh.second->Write();
        }
    }

    // this is the main event loop.  doEvent processes a single event iEvent...
    void make() {

        int single_event = cfg.get<int>("Input:event", -1);

        if (single_event >= 0) {
            doEvent(single_event);
            return;
        }

        unsigned long long firstEvent = cfg.get<unsigned long long>("Input:first-event", 0);

        if (cfg.exists("Input:max-events")) {
            unsigned long long maxEvents = cfg.get<unsigned long long>("Input:max-events");

            if (nEvents > maxEvents)
                nEvents = maxEvents;
        }

        // loop over events
        LOG_F(INFO, "Looping on %llu events starting from event %llu", nEvents, firstEvent);

        for (unsigned long long iEvent = firstEvent; iEvent < firstEvent + nEvents; iEvent++) {
            doEvent(iEvent);
        }

        trackFitter->showEvents();
        qPlotter->finish();
        writeEventHistograms();
    }

    Seed_t::iterator findHitById(Seed_t &track, unsigned int _id) {
        for (Seed_t::iterator it = track.begin(); it != track.end(); ++it) {
            KiTrack::IHit *h = (*it);

            if (static_cast<FwdHit *>(h)->_id == _id)
                return it;
        }

        return track.end();
    }

    void removeHits(std::map<int, std::vector<KiTrack::IHit *>> &hitmap, std::vector<Seed_t> &tracks) {
        LOG_SCOPE_FUNCTION(INFO);

        for (auto track : tracks) {
            if (track.size() > 0) {
                for (auto hit : track) {
                    int sector = hit->getSector();

                    // auto hitit = findHitById( hitmap[sector], static_cast<FwdHit*>(hit)->_id );
                    auto hitit = std::find(hitmap[sector].begin(), hitmap[sector].end(), hit);

                    if (hitit == hitmap[sector].end()) {
                        LOG_F(ERROR, "Hit on track but not in hitmap!");
                    } else {
                        hitmap[sector].erase(hitit);
                        totalHitsRemoved++;
                    }

                } // loop on hits in track
            }     // if track has 7 hits
        }         // loop on track

        return;
    } // removeHits

    void doEvent(unsigned long long int iEvent = 0) {
        LOG_SCOPE_FUNCTION(INFO);
        LOG_F(INFO, "/******************************EVENT START ********************************/");
        LOG_F(INFO, "iEvent = %llu", iEvent);

        /************** Cleanup **************************/
        // Moved cleanup to the start of doEvent, so that the fit results
        // persist after the call
        recoTracks.clear();
        recoTrackQuality.clear();
        recoTrackIdTruth.clear();
        fitMoms.clear();
        fitStatus.clear();

        // Clear pointers to the track reps from previous event
        for (auto p : _globalTrackReps)
            delete p;

        _globalTrackReps.clear();

        // Clear pointers to global tracks
        for (auto p : _globalTracks)
            delete p;

        _globalTracks.clear();
        /************** Cleanup **************************/

        qPlotter->startEvent(); // starts the timer for this event

        totalHitsRemoved = 0;

        /*************************************************************/
        // Step 1
        // Load and sort the hits
        /*************************************************************/
        std::map<int, std::vector<KiTrack::IHit *>> hitmap;

        fillHistograms();

        hitmap = hitLoader->load(iEvent);
        std::map<int, shared_ptr<McTrack>> &mcTrackMap = hitLoader->getMcTrackMap();

        bool mcTrackFinding = true;

        if (cfg.exists("TrackFinder"))
            mcTrackFinding = false;

        if (mcTrackFinding) {
            doMcTrackFinding(mcTrackMap);

            /***********************************************/
            // REFIT with Silicon hits
            if (cfg.get<bool>("TrackFitter:refitSi", true)) {
                LOG_SCOPE_F(INFO, "Refitting with Si hits (MC association)");
                addSiHitsMc();
                LOG_F(INFO, "Finished adding Si hits");
            } else {
                LOG_F(INFO, "Skipping Si Refit");
            }
            /***********************************************/

            qPlotter->summarizeEvent(recoTracks, mcTrackMap, fitMoms, fitStatus);
            return;
        }

        size_t nIterations = cfg.get<size_t>("TrackFinder:nIterations", 0);
        LOG_F(INFO, "Running %lu Tracking Iterations", nIterations);

        for (size_t iIteration = 0; iIteration < nIterations; iIteration++) {
            doTrackIteration(iIteration, hitmap);
        }

        /***********************************************/
        // REFIT with Silicon hits
        if (cfg.get<bool>("TrackFitter:refitSi", true)) {
            LOG_SCOPE_F(INFO, "Refitting");
            addSiHits();
            LOG_F(INFO, "Finished adding Si hits");
        } else {
            LOG_F(INFO, "Skipping Si Refit");
        }
        /***********************************************/

        qPlotter->summarizeEvent(recoTracks, mcTrackMap, fitMoms, fitStatus);
    } // doEvent

    void trackFitting(Seed_t &track) {
        LOG_SCOPE_FUNCTION(INFO);

        hist["FitStatus"]->Fill("Seeds", 1);

        // Calculate the MC info first and check filters
        int idt = 0;
        float qual = 0;
        idt = MCTruthUtils::domCon(track, qual);
        recoTrackQuality.push_back(qual);
        recoTrackIdTruth.push_back(idt);
        TVector3 mcSeedMom;

        auto mctm = hitLoader->getMcTrackMap();

        if (qual < cfg.get<float>("TrackFitter.McFilter:quality-min", 0.0)) {
            LOG_F(INFO, "McFilter: Skipping low quality (q=%f) track", qual);
            return;
        }
        if (mctm.count(idt)) {
            auto mct = mctm[idt];
            mcSeedMom.SetPtEtaPhi(mct->_pt, mct->_eta, mct->_phi);
            if (mct->_pt < cfg.get<float>("TrackFitter.McFilter:pt-min", 0.0) ||
                mct->_pt > cfg.get<float>("TrackFitter.McFilter:pt-max", 1e10)) {
                LOG_F(INFO, "McFilter: Skipping low pt (pt=%f) track", mct->_pt);
                return;
            }
            if (mct->_eta < cfg.get<float>("TrackFitter.McFilter:eta-min", 0.0) ||
                mct->_eta > cfg.get<float>("TrackFitter.McFilter:eta-max", 1e10)) {
                LOG_F(INFO, "McFilter: Skipping low eta (eta=%f) track", mct->_eta);
                return;
            }
            LOG_F(INFO, "Checking McFilter on track id=%d, quality=%f, (%f, %f, %f), charge=%d", idt, qual, mct->_pt, mct->_eta, mct->_phi, mct->_q);
        } else {
            LOG_F(INFO, "Cannot find the McTrack ID = %d ", idt);
        }

        // Done with Mc Filter

        if (doTrackFitting) {
            hist["FitStatus"]->Fill("AttemptFit", 1);

            TVector3 p;
            if (true == cfg.get<bool>("TrackFitter:mcSeed", false)) {
                p = trackFitter->fitTrack(track, 0, &mcSeedMom);
            } else {
                p = trackFitter->fitTrack(track);
            }

            if (p.Perp() > 1e-3) {
                hist["FitStatus"]->Fill("GoodFit", 1);
            } else {
                hist["FitStatus"]->Fill("BadFit", 1);
            }

            fitMoms.push_back(p);
            fitStatus.push_back(trackFitter->getStatus());

            auto ft = trackFitter->getTrack();
            if (ft->getFitStatus(ft->getCardinalRep())->isFitConverged() && p.Perp() > 1e-3) {
                hist["FitStatus"]->Fill("GoodCardinal", 1);
            }

            // Clone the track rep
            _globalTrackReps.push_back(trackFitter->getTrackRep()->clone());
            genfit::Track *mytrack = new genfit::Track(*trackFitter->getTrack());
            float qatruth;
            int idtruth = MCTruthUtils::domCon(track, qatruth);
            mytrack->setMcTrackId(idtruth);
            _globalTracks.push_back(mytrack);
        } else {
            LOG_F(INFO, "Skipping Track Fitting");
        }
    }

    void doMcTrackFinding(std::map<int, shared_ptr<McTrack>> mcTrackMap) {

        qPlotter->startIteration();

        // we will build reco tracks from each McTrack
        for (auto kv : mcTrackMap) {
            auto mc_track = kv.second;

            if (mc_track->hits.size() < 4)
                continue;

            std::set<size_t> uvid;
            Seed_t track;

            for (auto h : mc_track->hits) {
                track.push_back(h);
                uvid.insert(static_cast<FwdHit *>(h)->_vid);
            }

            if (uvid.size() == track.size()) { // only add tracks that have one hit per volume
                recoTracks.push_back(track);
                int idt = 0;
                float qual = 0;
                MCTruthUtils::domCon(track, qual);
                recoTrackQuality.push_back(qual);
                recoTrackIdTruth.push_back(idt);
            }
        }

        LOG_F(INFO, "Made %lu Reco Tracks from MC Tracks", recoTracks.size());

        long long itStart = loguru::now_ns();
        // Fit each accepted track seed
        for (auto t : recoTracks) {
            trackFitting(t);
        }
        long long itEnd = loguru::now_ns();
        long long duration = (itEnd - itStart) * 1e-6; // milliseconds
        this->hist["FitDuration"]->Fill(duration);

        qPlotter->afterIteration(0, recoTracks);
    }


    /**
    * @brief Slices a hitmap into a phi section
    * 
    * @param inputMap INPUT hitmap to process
    * @param outputMap OUTPUT hitmap, will be cleared and filled with only the hits from inputMap that are within phi region
    * @param phi_min The minimum phi to accept
    * @param phi_max The maximum Phi to accept
    * 
    * @returns The number of hits in the outputMap
    */
    size_t sliceHitMapInPhi( std::map<int, std::vector<KiTrack::IHit*> > &inputMap, std::map<int, std::vector<KiTrack::IHit*> > &outputMap, float phi_min, float phi_max ){
        size_t n_hits_kept = 0;

        outputMap.clear(); // child STL containers will get cleared too
        for ( auto kv : inputMap ){
            for ( KiTrack::IHit* hit : kv.second ){
                TVector3 vec(hit->getX(), hit->getY(), hit->getZ() );
                if ( vec.Phi() < phi_min || vec.Phi() > phi_max ) continue;

                // now add the hits to the sliced map
                outputMap[kv.first].push_back( hit );
                n_hits_kept ++;
            } // loop on hits
        } // loop on map
        return n_hits_kept;
    }

    vector<Seed_t> doTrackingOnHitmapSubset( size_t iIteration, std::map<int, std::vector<KiTrack::IHit*> > &hitmap  ) {
        LOG_SCOPE_FUNCTION(INFO);
        /*************************************************************/
        // Step 2
        // build 2-hit segments (setup parent child relationships)
        /*************************************************************/
        // Initialize the segment builder with sorted hits
        KiTrack::SegmentBuilder builder(hitmap);

        // Load the criteria used for 2-hit segments
        // This loads from XML config if available
        std::string criteriaPath = "TrackFinder.Iteration[" + std::to_string(iIteration) + "].SegmentBuilder";

        if (false == cfg.exists(criteriaPath)) {
            // Use the default for all iterations if it is given.
            // If not then no criteria will be applied
            criteriaPath = "TrackFinder.SegmentBuilder";
        }

        twoHitCrit.clear();
        twoHitCrit = loadCriteria(criteriaPath);
        builder.addCriteria(twoHitCrit);

        // Setup the connector (this tells it how to connect hits together into segments)
        std::string connPath = "TrackFinder.Iteration[" + std::to_string(iIteration) + "].Connector";

        if (false == cfg.exists(connPath))
            connPath = "TrackFinder.Connector";

        unsigned int distance = cfg.get<unsigned int>(connPath + ":distance", 1);
        LOG_F(INFO, "Connector( distance = %u )", distance);
        FwdConnector connector(distance);
        builder.addSectorConnector(&connector);

        // Get the segments and return an automaton object for further work
        LOG_F(INFO, "Getting the 1 hit segments");
        KiTrack::Automaton automaton = builder.get1SegAutomaton();

        // at any point we can get a list of tracks out like this:
        // std::vector < std::vector< KiTrack::IHit* > > tracks = automaton.getTracks();
        // we can apply an optional parameter <nHits> to only get tracks with >=nHits in them

        // Report the number of segments and connections after the first step
        LOG_F(INFO, "nSegments=%lu", automaton.getSegments().size());
        LOG_F(INFO, "nConnections=%u", automaton.getNumberOfConnections());

        /*************************************************************/
        // Step 3
        // build 3-hit segments from the 2-hit segments
        /*************************************************************/
        automaton.clearCriteria();
        automaton.resetStates();
        criteriaPath = "TrackFinder.Iteration[" + std::to_string(iIteration) + "].ThreeHitSegments";

        if (false == cfg.exists(criteriaPath))
            criteriaPath = "TrackFinder.ThreeHitSegments";

        threeHitCrit.clear();
        threeHitCrit = loadCriteria(criteriaPath);
        automaton.addCriteria(threeHitCrit);
        automaton.lengthenSegments();

        bool doAutomation = cfg.get<bool>(criteriaPath + ":doAutomation", true);
        bool doCleanBadStates = cfg.get<bool>(criteriaPath + ":cleanBadStates", true);

        if (doAutomation) {
            LOG_F(INFO, "Doing Automation Step");
            automaton.doAutomaton();
        } else {
            LOG_F(INFO, "Not running Automation Step");
        }

        if (doAutomation && doCleanBadStates) {
            automaton.cleanBadStates();
        }

        LOG_F(INFO, "nSegments=%lu", automaton.getSegments().size());
        LOG_F(INFO, "nConnections=%u", automaton.getNumberOfConnections());

        /*************************************************************/
        // Step 4
        // Get the tracks from the possible tracks that are the best subset
        /*************************************************************/
        std::string subsetPath = "TrackFinder.Iteration[" + std::to_string(iIteration) + "].SubsetNN";

        if (false == cfg.exists(subsetPath))
            subsetPath = "TrackFinder.SubsetNN";

        //  only for debug really
        bool findSubsets = cfg.get<bool>(subsetPath + ":active", true);
        std::vector<Seed_t> acceptedTracks;
        std::vector<Seed_t> rejectedTracks;

        if (findSubsets) {
            LOG_SCOPE_F(INFO, "SubsetNN");
            LOG_F(INFO, "Trying to get best set of tracks given all the possibilities");

            size_t minHitsOnTrack = cfg.get<size_t>(subsetPath + ":min-hits-on-track", 7);
            LOG_F(INFO, "Getting all tracks with at least %lu hits on them", minHitsOnTrack);
            std::vector<Seed_t> tracks = automaton.getTracks(minHitsOnTrack);
            LOG_F(INFO, "We have %lu Tracks to work with", tracks.size());

            float omega = cfg.get<float>(subsetPath + ".Omega", 0.75);
            float stableThreshold = cfg.get<float>(subsetPath + ".StableThreshold", 0.1);
            float Ti = cfg.get<float>(subsetPath + ".InitialTemp", 2.1);
            float Tf = cfg.get<float>(subsetPath + ".InfTemp", 0.1);

            LOG_F(INFO, "SubsetHopfiledNN Settings:");
            LOG_F(INFO, "Temp (initial=%0.3f, inf=%0.3f)", Ti, Tf);
            LOG_F(INFO, "Omega=%0.3f", omega);
            LOG_F(INFO, "StableThreshold=%0.3f", stableThreshold);

            KiTrack::SubsetHopfieldNN<Seed_t> subset;
            subset.add(tracks);
            subset.setOmega(omega);
            subset.setLimitForStable(stableThreshold);
            subset.setTStart(Ti);

            SeedCompare comparer;
            SeedQual quality;

            subset.calculateBestSet(comparer, quality);

            acceptedTracks = subset.getAccepted();
            rejectedTracks = subset.getRejected();

            LOG_F(INFO, "We had %lu tracks. Accepted = %lu, Rejected = %lu", tracks.size(), acceptedTracks.size(), rejectedTracks.size());

        } else { // the subset and hit removal
            LOG_F(INFO, "The SubsetNN Step is turned OFF. This also means the Hit Remover is turned OFF (requires SubsetNN step)");

            size_t minHitsOnTrack = cfg.get<size_t>(subsetPath + ":min-hits-on-track", 7);
            LOG_F(INFO, "Getting all tracks with at least %lu hits on them", minHitsOnTrack);
            acceptedTracks = automaton.getTracks(minHitsOnTrack);
            LOG_F(INFO, "We have %lu Tracks to work with", acceptedTracks.size());

            // qPlotter->afterIteration(iIteration, tracks);
        }// subset off

        return acceptedTracks;
    } // doTrackingOnHitmapSubset

    void doTrackIteration(size_t iIteration, std::map<int, std::vector<KiTrack::IHit *>> &hitmap) {
        LOG_SCOPE_FUNCTION(INFO);
        LOG_F(INFO, "Tracking Iteration %lu", iIteration);

        // empty the list of reco tracks for the iteration
        recoTracksThisItertion.clear();

        // check to see if we have hits!
        size_t nHitsThisIteration = nHitsInHitMap(hitmap);

        if (nHitsThisIteration < 4) {
            LOG_F(INFO, "No hits left in the hitmap! Skipping this iteration");
            return;
        } else {
            LOG_F(INFO, "Working with %lu hits this iteration", nHitsThisIteration);
        }

        // this starts the timer for the iteration
        qPlotter->startIteration();


        if ( false ) { // no phi slicing!
            /*************************************************************/
            // Steps 2 - 4 here
            /*************************************************************/
            auto acceptedTracks = doTrackingOnHitmapSubset( iIteration, hitmap );
            recoTracksThisItertion.insert( recoTracksThisItertion.end(), acceptedTracks.begin(), acceptedTracks.end() );
        } else {

            std::map<int, std::vector<KiTrack::IHit*> > slicedHitMap;
            std::string pslPath = "TrackFinder.Iteration["+ std::to_string(iIteration) + "]:nPhiSlices";
            if ( false == cfg.exists( pslPath ) ) pslPath = "TrackFinder:nPhiSlices";
            size_t phi_slice_count = cfg.get<size_t>( pslPath, 1 );

            if ( phi_slice_count == 0 || phi_slice_count > 100 ){
                LOG_F( WARNING, "Invalid phi_slice_count = %lu, resetting to 1", phi_slice_count);
                phi_slice_count= 1;
            }

            LOG_F( INFO, "Using %lu phi_slices", phi_slice_count );
            float phi_slice = 2 * TMath::Pi() / (float) phi_slice_count;
            for ( size_t phi_slice_index = 0; phi_slice_index < phi_slice_count; phi_slice_index++ ){

                float phi_min = phi_slice_index * phi_slice - TMath::Pi();
                float phi_max = (phi_slice_index + 1) * phi_slice - TMath::Pi();
                LOG_F( INFO, "Working with hits in %0.2f < phi < %0.2f", phi_min, phi_max );

                /*************************************************************/
                // Step 1A
                // Slice the hitmap into a phi section if needed
                // If we do that, check again that we arent wasting time on empty sections
                /*************************************************************/
                size_t nHitsThisSlice = 0;
                if ( phi_slice_count > 1 ){
                    nHitsThisSlice = sliceHitMapInPhi( hitmap, slicedHitMap, phi_min, phi_max );
                    if ( nHitsThisSlice < 4 ) {
                        LOG_F( INFO, "This phi ( %f < phi < %f ) only has %lu hits, Skipping this Slice", phi_min, phi_max, nHitsThisSlice );
                        continue;
                    } else {
                        LOG_F( INFO, "Working with %lu hits this Slice", nHitsThisSlice );
                    }
                } else { // no need to slice
                    // I think this incurs a copy, maybe we can find a way to avoid.
                    slicedHitMap = hitmap;
                }
                
                /*************************************************************/
                // Steps 2 - 4 here
                /*************************************************************/
                auto acceptedTracks = doTrackingOnHitmapSubset( iIteration, slicedHitMap );
                recoTracksThisItertion.insert( recoTracksThisItertion.end(), acceptedTracks.begin(), acceptedTracks.end() );
            } //loop on phi slices
        }// if loop on phi slices

        /*************************************************************/
        // Step 5
        // Remove the hits from any track that was found
        /*************************************************************/
        std::string hrmPath = "TrackFinder.Iteration["+ std::to_string(iIteration) + "].HitRemover";
        if ( false == cfg.exists( hrmPath ) ) hrmPath = "TrackFinder.HitRemover";

        if ( true == cfg.get<bool>( hrmPath + ":active", true ) ){
            LOG_F( INFO, "Removing hits, BEFORE n = %lu", nHitsInHitMap( hitmap ) );
            removeHits( hitmap, recoTracksThisItertion );
            LOG_F( INFO, "Removing hits, AFTER n = %lu", nHitsInHitMap( hitmap ) );
        } else {
            LOG_F( INFO, "Hit Remover is turned OFF" );
        }


        // doTrackFitting( recoTracksThisItertion );

        for (auto t : recoTracksThisItertion) {
            trackFitting(t);
        }

        qPlotter->afterIteration( iIteration, recoTracksThisItertion );

        // Add the set of all accepted tracks (this iteration) to our collection of found tracks from all iterations
        recoTracks.insert( recoTracks.end(), recoTracksThisItertion.begin(), recoTracksThisItertion.end() );

    } // doTrackIteration

    void addSiHitsMc() {
        LOG_SCOPE_FUNCTION(INFO);
        std::map<int, std::vector<KiTrack::IHit *>> hitmap = hitLoader->loadSi(0);
        LOG_F(INFO, "hitmap size = %lu (should be 3)", hitmap.size());

        LOG_F(INFO, "We have %d global tracks to work with", _globalTracks.size());
        for (size_t i = 0; i < _globalTracks.size(); i++) {
            LOG_F(INFO, "_globalTracks mcTrackId = %d", _globalTracks[i]->getMcTrackId());

            if (_globalTracks[i]->getFitStatus(_globalTracks[i]->getCardinalRep())->isFitConverged() == false || fitMoms[i].Perp() < 1e-3) {
                LOG_F(WARNING, "Original Track fit did not converge, skipping");
                return;
            }

            hist["FitStatus"]->Fill("PossibleReFit", 1);

            std::vector<KiTrack::IHit *> si_hits_for_this_track(3, nullptr);

            for (size_t j = 0; j < 3; j++) {
                LOG_F(INFO, "Checking hitmap[%lu]", j);
                for (auto h0 : hitmap[j]) {
                    LOG_F(INFO, "Checking hit with _tid=%lu", dynamic_cast<FwdHit *>(h0)->_tid);
                    if (dynamic_cast<FwdHit *>(h0)->_tid == _globalTracks[i]->getMcTrackId()) {
                        si_hits_for_this_track[j] = h0;
                        LOG_F(INFO, "Found Si hit on layer %lu", j);
                        break;
                    }
                } // loop on hits in this layer of hitmap
            }     // loop on hitmap layers

            LOG_F(INFO, "Si hit0 = %p", si_hits_for_this_track[0]);
            LOG_F(INFO, "Si hit1 = %p", si_hits_for_this_track[1]);
            LOG_F(INFO, "Si hit2 = %p", si_hits_for_this_track[2]);

            size_t nSiHitsFound = 0;
            if ( si_hits_for_this_track[0] != nullptr ) nSiHitsFound++;
            if ( si_hits_for_this_track[1] != nullptr ) nSiHitsFound++;
            if ( si_hits_for_this_track[2] != nullptr ) nSiHitsFound++;
            LOG_F( INFO, "nSiHitsFound = %lu", nSiHitsFound );

            this->hist[ "nSiHitsFound" ]->Fill( 1, ( si_hits_for_this_track[0] != nullptr ? 1 : 0 ) );
            this->hist[ "nSiHitsFound" ]->Fill( 2, ( si_hits_for_this_track[1] != nullptr ? 1 : 0 ) );
            this->hist[ "nSiHitsFound" ]->Fill( 3, ( si_hits_for_this_track[2] != nullptr ? 1 : 0 ) );

            bool have_all_three = (si_hits_for_this_track[0] != nullptr) && (si_hits_for_this_track[1] != nullptr) && (si_hits_for_this_track[2] != nullptr);

            if (nSiHitsFound >= 1) {
                hist["FitStatus"]->Fill("AttemptReFit", 1);
                TVector3 p = trackFitter->refitTrackWithSiHits(_globalTracks[i], si_hits_for_this_track);

                if (p.Perp() == fitMoms[i].Perp()) {
                    hist["FitStatus"]->Fill("BadReFit", 1);

                } else {
                    hist["FitStatus"]->Fill("GoodReFit", 1);
                }

                LOG_F(INFO, "Global track now has: %lu points", _globalTracks[i]->getNumPoints());
                LOG_F(INFO, "pt was: %0.2f and now is: %0.2f", fitMoms[i].Perp(), p.Perp());
                fitMoms[i] = p;
            } // we have 3 Si hits to refit with

            hist["FitStatus"]->Fill( TString::Format( "w%luSi", nSiHitsFound ).Data(), 1 );

        }     // loop on the global tracks
    }         // ad Si hits via MC associations

    void addSiHits() {
        LOG_SCOPE_FUNCTION(INFO);
        std::map<int, std::vector<KiTrack::IHit *>> hitmap = hitLoader->loadSi(0);

        LOG_F(INFO, "hitmap size = %lu (should be 3)", hitmap.size());

        // loop on global tracks
        for (size_t i = 0; i < _globalTracks.size(); i++) {

            if (_globalTracks[i]->getFitStatus(_globalTracks[i]->getCardinalRep())->isFitConverged() == false) {
                LOG_F(WARNING, "Original Track fit did not converge, skipping");
                return;
            }

            hist["FitStatus"]->Fill("PossibleReFit", 1);

            std::vector<KiTrack::IHit *> hits_near_disk0;
            std::vector<KiTrack::IHit *> hits_near_disk1;
            std::vector<KiTrack::IHit *> hits_near_disk2;
            try {
                auto msp2 = trackFitter->projectTo(2, _globalTracks[i]);
                auto msp1 = trackFitter->projectTo(1, _globalTracks[i]);
                auto msp0 = trackFitter->projectTo(0, _globalTracks[i]);

                // now look for Si hits near these
                hits_near_disk2 = findSiHitsNearMe(hitmap[2], msp2);
                hits_near_disk1 = findSiHitsNearMe(hitmap[1], msp1);
                hits_near_disk0 = findSiHitsNearMe(hitmap[0], msp0);
            } catch (genfit::Exception &e) {
                LOG_F(ERROR, "Failed to project to Si disk: %s", e.what());
            }

            LOG_F(INFO, "There are (%lu, %lu, %lu) hits near the track on Si disks 0, 1, 2", hits_near_disk0.size(), hits_near_disk1.size(), hits_near_disk2.size());
            LOG_F(INFO, "Track already has %lu points", _globalTracks[i]->getNumPoints());
            vector<KiTrack::IHit *> hits_to_add;

            size_t nSiHitsFound = 0; // this is really # of disks on which a hit is found

            this->hist[ "nSiHitsFound" ]->Fill( 1, hits_near_disk0.size() );
            this->hist[ "nSiHitsFound" ]->Fill( 2, hits_near_disk1.size() );
            this->hist[ "nSiHitsFound" ]->Fill( 3, hits_near_disk2.size() );

            //  TODO: HANDLE multiple points found?
            if ( hits_near_disk0.size() == 1 ) {
                hits_to_add.push_back( hits_near_disk0[0] );
                nSiHitsFound++;
            } else {
                hits_to_add.push_back( nullptr );
            }
            if ( hits_near_disk1.size() == 1 ) {
                hits_to_add.push_back( hits_near_disk1[0] );
                nSiHitsFound++;
            } else {
                hits_to_add.push_back( nullptr );
            }
            if ( hits_near_disk2.size() == 1 ) {
                hits_to_add.push_back( hits_near_disk2[0] );
                nSiHitsFound++;
            } else {
                hits_to_add.push_back( nullptr );
            }

            if (nSiHitsFound >= 1) {
                LOG_SCOPE_F( INFO, "attempting to Refit with %lu si hits", nSiHitsFound );

                hist["FitStatus"]->Fill("AttemptReFit", 1);
                TVector3 p = trackFitter->refitTrackWithSiHits(_globalTracks[i], hits_to_add);

                if (p.Perp() == fitMoms[i].Perp()) {
                    hist["FitStatus"]->Fill("BadReFit", 1);

                } else {
                    hist["FitStatus"]->Fill("GoodReFit", 1);

                    fitMoms[i] = p;
                }

                // LOG_F(INFO, "Global track now has: %lu point", _globalTracks[i]->getNumPoints());
                // LOG_F(INFO, "pt was: %0.2f and now is: %0.2f", fitMoms[i].Perp(), p.Perp());

            } else {
                // fitMoms[ i ] = TVector3( 1000, 1000, 1000 );
            }

            hist["FitStatus"]->Fill( TString::Format( "w%luSi", nSiHitsFound ).Data(), 1 );

        } // loop on globals
    }     // addSiHits

    std::vector<KiTrack::IHit *> findSiHitsNearMe(std::vector<KiTrack::IHit *> &available_hits, genfit::MeasuredStateOnPlane &msp, double dphi = 0.004 * 15.5, double dr = 0.75) {
        LOG_SCOPE_FUNCTION(INFO);
        double probe_phi = TMath::ATan2(msp.getPos().Y(), msp.getPos().X());
        double probe_r = sqrt(pow(msp.getPos().X(), 2) + pow(msp.getPos().Y(), 2));

        std::vector<KiTrack::IHit *> found_hits;

        for (auto h : available_hits) {
            double h_phi = TMath::ATan2(h->getY(), h->getX());
            double h_r = sqrt(pow(h->getX(), 2) + pow(h->getY(), 2));
            double mdphi = fabs(h_phi - probe_phi);
            if ( mdphi > 2 * TMath::Pi() ) {
                LOG_F( WARNING, "BAD WRAP" );
            }
            LOG_F(1, "hit_phi=%0.2f - mphi=%0.2f = %0.2f", h_phi, probe_phi, fabs(h_phi - probe_phi));
            if ( mdphi < dphi || fabs( h_r - probe_r ) < dr) { // handle 2pi edge
                found_hits.push_back(h);
            }
        }

        return found_hits;
    }

    bool getSaveCriteriaValues() { return saveCriteriaValues; }
    std::vector<KiTrack::ICriterion *> getTwoHitCriteria() { return twoHitCrit; }
    std::vector<KiTrack::ICriterion *> getThreeHitCriteria() { return threeHitCrit; }

    TrackFitter *getTrackFitter() { return trackFitter; }

  protected:
    FastSim *fastSim;
    TTree *tree;
    TFile *fInput;
    unsigned long long int nEvents;

    bool doTrackFitting = true;
    bool saveCriteriaValues = false;

    /* TTree data members */
    int tree_n;
    const static unsigned int tree_max_n = 5000;
    int tree_vid[tree_max_n], tree_tid[tree_max_n];
    float tree_x[tree_max_n], tree_y[tree_max_n], tree_z[tree_max_n], tree_pt[tree_max_n];

    jdb::XmlConfig cfg;
    map<string, string> cmdLineConfig;
    std::string configFile;
    // event level summary histograms
    // map<string, TH1 *> histograms;

    vector<size_t> trueTracksWithNHits;
    float total7HitTrue;
    float total7HitFound;
    size_t totalHitsRemoved;
    size_t nTrueTracks;
    size_t nTrueTracksWith7;

    std::vector<Seed_t> recoTracks; // the tracks recod from all iterations
    std::vector<Seed_t> recoTracksThisItertion;
    std::vector<float> recoTrackQuality;
    std::vector<int> recoTrackIdTruth;
    std::vector<TVector3> fitMoms;
    // vector<int> fitQs;
    std::vector<genfit::FitStatus> fitStatus;
    std::vector<genfit::AbsTrackRep *> _globalTrackReps;
    std::vector<genfit::Track *> _globalTracks;

    QualityPlotter *qPlotter;
    IHitLoader *hitLoader;

    TrackFitter *trackFitter = nullptr;

    std::vector<KiTrack::ICriterion *> twoHitCrit;
    std::vector<KiTrack::ICriterion *> threeHitCrit;

    // histograms of the raw input data
    std::map<std::string, TH1 *> hist;
    std::map<std::string, std::vector<float>> criteriaValues;

  public:
    const std::vector<Seed_t> &getRecoTracks() const { return recoTracks; }
    const std::vector<TVector3> &getFitMomenta() const { return fitMoms; }
    const std::vector<genfit::FitStatus> &getFitStatus() const { return fitStatus; }
    const std::vector<genfit::AbsTrackRep *> &globalTrackReps() const { return _globalTrackReps; }
    const std::vector<genfit::Track *> &globalTracks() const { return _globalTracks; }
};

#endif
