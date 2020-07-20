#define LOGURU_IMPLEMENTATION 1

#include "StgMaker/StgMaker.h"
#include "StgMaker/include/Tracker/FwdHit.h"
#include "StgMaker/include/Tracker/FwdTracker.h"
#include "StgMaker/include/Tracker/TrackFitter.h"

#include "KiTrack/IHit.h"

#include "TMath.h"

#include <limits>
#include <map>
#include <string>

#include "StEvent.h"
#include "StGlobalTrack.h"
#include "StHelixModel.h"
#include "StPrimaryTrack.h"
#include "StRnDHit.h"
#include "StRnDHitCollection.h"
#include "StTrack.h"
#include "StTrackGeometry.h"
#include "StTrackNode.h"

#include "tables/St_g2t_fts_hit_Table.h"
#include "tables/St_g2t_track_Table.h"

#include "StarMagField.h"

#include "StEventUtilities/StEventHelper.h"

#include "StEvent/StEnumerations.h"
#include "StTrackDetectorInfo.h"

#include "St_base/StMessMgr.h"
#include "StarClassLibrary/StPhysicalHelix.hh"

#include "TROOT.h"
#include "TLorentzVector.h"

//_______________________________________________________________________________________
// For now, accept anything we are passed, no matter what it is or how bad it is
template <typename T>
bool accept(T) { return true; }

template <>
bool accept(genfit::Track *track) {

    // This also gets rid of failed fits (but may need to explicitly
    // for fit failure...)
    if (track->getNumPoints() <= 0)
        return false; // fit may have failed

    // Fitted state at the first point
    const auto &atFirstPoint = track->getFittedState();

    TVector3 momentum = atFirstPoint.getMom();
    double pt = momentum.Perp();

    if (pt < 0.10)
        return false; // below this

    return true;
};

class SiRasterizer {
  public:
    SiRasterizer() {}
    SiRasterizer(jdb::XmlConfig &_cfg) { setup(_cfg); }
    ~SiRasterizer() {}
    void setup(jdb::XmlConfig &_cfg) {
        cfg = _cfg;

        raster_r = cfg.get<double>("SiRasterizer:r", 3.0);
        raster_phi = cfg.get<double>("SiRasterizer:phi", 0.1);
        if (active())
            LOG_F(INFO, "SiRasterizer (active) r=%f, phi=%f", raster_r, raster_phi);
        else {
            LOG_F(INFO, "SiRasterizer (inactive)");
        }
    }

    bool active() {
        return cfg.get<bool>("SiRasterizer:active", false);
    }

    TVector3 raster(TVector3 _p) {
        TVector3 p = _p;
        double r = p.Perp();
        double phi = p.Phi();
        // 5.0 is the r minimum of the Si
        p.SetPerp(5.0 + (floor((r - 5.0) / raster_r) * raster_r + raster_r / 2.0));
        p.SetPhi(-TMath::Pi() + (floor((phi + TMath::Pi()) / raster_phi) * raster_phi + raster_phi / 2.0));
        return p;
    }

    jdb::XmlConfig cfg;
    double raster_r, raster_phi;
};

//_______________________________________________________________________________________
// Adaptor for STAR magnetic field
class StarFieldAdaptor : public genfit::AbsBField {
  public:
    StarFieldAdaptor() {
        _gField = this; // global pointer in TrackFitter.h
    };
    virtual TVector3 get(const TVector3 &position) const {
        double x[] = {position[0], position[1], position[2]};
        double B[] = {0, 0, 0};

        if (StarMagField::Instance())
            StarMagField::Instance()->Field(x, B);

        return TVector3(B);
    };
    virtual void get(const double &_x, const double &_y, const double &_z, double &Bx, double &By, double &Bz) const {
        double x[] = {_x, _y, _z};
        double B[] = {0, 0, 0};

        if (StarMagField::Instance())
            StarMagField::Instance()->Field(x, B);

        Bx = B[0];
        By = B[1];
        Bz = B[2];
        return;
    };
};

//  Wrapper class around the forward tracker
class ForwardTracker : public ForwardTrackMaker {
  public:
    // Replaces original initialization.  Config file and hitloader
    // will be provided by the maker.
    void initialize() {
        LOG_INFO << "ForwardTracker::initialize()" << endm;
        nEvents = 1; // only process single event

        // Create the forward system...
        gFwdSystem = new FwdSystem(7);

        // make our quality plotter
        qPlotter = new QualityPlotter(cfg);
        LOG_INFO << "Booking histograms for nIterations=" << cfg.get<size_t>("TrackFinder:nIterations", 1) << endm;
        qPlotter->makeHistograms(cfg.get<size_t>("TrackFinder:nIterations", 1));

        // initialize the track fitter
        trackFitter = new TrackFitter(cfg);
        trackFitter->setup(cfg.get<bool>("TrackFitter:display"));

        ForwardTrackMaker::initialize();
    }

    void finish() {
        LOG_SCOPE_FUNCTION(INFO);
        qPlotter->finish();
        writeEventHistograms();
    }
};

// Wrapper around the hit load.
class ForwardHitLoader : public IHitLoader {
  public:
    unsigned long long nEvents() { return 1; }
    std::map<int, std::vector<KiTrack::IHit *>> &load(unsigned long long) {
        return _hits;
    };
    std::map<int, std::vector<KiTrack::IHit *>> &loadSi(unsigned long long) {
        return _fsi_hits;
    };
    std::map<int, shared_ptr<McTrack>> &getMcTrackMap() {
        return _mctracks;
    };

    // Cleanup
    void clear() {
        _hits.clear();
        _fsi_hits.clear();
        _mctracks.clear();
    }

    // TODO, protect and add interaface for pushing hits / tracks
    std::map<int, std::vector<KiTrack::IHit *>> _hits;
    std::map<int, std::vector<KiTrack::IHit *>> _fsi_hits;
    std::map<int, shared_ptr<McTrack>> _mctracks;
};

//________________________________________________________________________
StgMaker::StgMaker() : StMaker("stg"), mForwardTracker(0), mForwardHitLoader(0), mFieldAdaptor(new StarFieldAdaptor()){

                                                                                 };

int StgMaker::Finish() {
    LOG_SCOPE_FUNCTION(INFO);

    mForwardTracker->finish();

    gDirectory->mkdir("StgMaker");
    gDirectory->cd("StgMaker");
    for (auto nh : histograms) {
        nh.second->SetDirectory(gDirectory);
        nh.second->Write();
    }

    // gDirectory->cd();
    gDirectory->Write();

    if (mGenTree) {
        mlTree->Print();
        mlFile->cd();
        mlTree->Write();
        mlFile->Write();
    }

    return kStOk;
}

//________________________________________________________________________
int StgMaker::Init() {
    // Initialize configuration file
    std::string configFile = "config.xml";
    if (mConfigFile.length() > 4) {
        configFile = mConfigFile;
        LOG_F(INFO, "Config File : %s", mConfigFile.c_str());
    }
    std::map<string, string> cmdLineConfig;
    xfg.loadFile(configFile, cmdLineConfig);

    // setup the loguru log file
    loguru::add_file("everything.log", loguru::Truncate, loguru::Verbosity_2);
    loguru::g_stderr_verbosity = loguru::Verbosity_OFF;

    if (mGenTree) {
        mlFile = new TFile("mltree.root", "RECREATE");
        mlTree = new TTree("Stg", "stg hits");
        mlTree->Branch("n", &mlt_n, "n/I");
        mlTree->Branch("x", mlt_x, "x[n]/F");
        mlTree->Branch("y", mlt_y, "y[n]/F");
        mlTree->Branch("z", mlt_z, "z[n]/F");
        mlTree->Branch("tid", &mlt_tid, "tid[n]/I");
        mlTree->Branch("vid", &mlt_vid, "vid[n]/I");
        mlTree->Branch("hpt", &mlt_hpt, "hpt[n]/F");
        mlTree->Branch("hsv", &mlt_hsv, "hsv[n]/I");

        // mc tracks
        mlTree->Branch("nt", &mlt_nt, "nt/I");
        mlTree->Branch("pt", &mlt_pt, "pt[nt]/F");
        mlTree->Branch("eta", &mlt_eta, "eta[nt]/F");
        mlTree->Branch("phi", &mlt_phi, "phi[nt]/F");
        mlTree->Branch("tid", &mlt_tid, "tid/I");

        std::string path = "TrackFinder.Iteration[0].SegmentBuilder";
        std::vector<string> paths = xfg.childrenOf(path);

        for (string p : paths) {
            string name = xfg.get<string>(p + ":name");
            bool active = xfg.get<bool>(p + ":active", true);
            mlt_crits[name]; // create the entry
            mlTree->Branch(name.c_str(), &mlt_crits[name]);
            mlTree->Branch((name + "_trackIds").c_str(), &mlt_crit_track_ids[name]);
        }

        // Three hit criteria
        path = "TrackFinder.Iteration[0].ThreeHitSegments";
        paths = xfg.childrenOf(path);

        for (string p : paths) {
            string name = xfg.get<string>(p + ":name");
            bool active = xfg.get<bool>(p + ":active", true);
            mlt_crits[name]; // create the entry
            mlTree->Branch(name.c_str(), &mlt_crits[name]);
            mlTree->Branch((name + "_trackIds").c_str(), &mlt_crit_track_ids[name]);
        }

        mlTree->SetAutoFlush(0);
    }

    mSiRasterizer = new SiRasterizer(xfg);

    mForwardTracker = new ForwardTracker();
    mForwardTracker->setConfig(xfg);
    // only save criteria values if we are generating a tree.
    mForwardTracker->setSaveCriteriaValues(mGenTree);

    mForwardHitLoader = new ForwardHitLoader();
    mForwardTracker->setLoader(mForwardHitLoader);
    mForwardTracker->initialize();

    // create histograms
    histograms["nMcTracks"] = new TH1I("nMcTracks", ";# MC Tracks/Event", 1000, 0, 1000);
    histograms["nHitsSTGC"] = new TH1I("nHitsSTGC", ";# STGC Hits/Event", 1000, 0, 1000);
    histograms["nHitsFSI"] = new TH1I("nHitsFSI", ";# FSIT Hits/Event", 1000, 0, 1000);

    histograms["stgc_volume_id"] = new TH1I("stgc_volume_id", ";stgc_volume_id", 50, 0, 50);
    histograms["fsi_volume_id"] = new TH1I("fsi_volume_id", ";fsi_volume_id", 50, 0, 50);

    histograms["fsiHitDeltaR"] = new TH1F("fsiHitDeltaR", "FSI; delta r (cm); ", 500, -5, 5);
    histograms["fsiHitDeltaPhi"] = new TH1F("fsiHitDeltaPhi", "FSI; delta phi; ", 500, -5, 5);

    // there are 4 stgc stations
    for (int i = 0; i < 4; i++) {
        histograms[TString::Format("stgc%dHitMap", i).Data()] = new TH2F(TString::Format("stgc%dHitMap", i), TString::Format("STGC Layer %d; x (cm); y(cm)"), 200, -100, 100, 200, -100, 100);
    }

    // There are 3 silicon stations
    for (int i = 0; i < 3; i++) {
        histograms[TString::Format("fsi%dHitMap", i).Data()] = new TH2F(TString::Format("fsi%dHitMap", i), TString::Format("FSI Layer %d; x (cm); y(cm)"), 200, -100, 100, 200, -100, 100);
        histograms[TString::Format("fsi%dHitMapR", i).Data()] = new TH1F(TString::Format("fsi%dHitMapR", i), TString::Format("FSI Layer %d; r (cm); "), 500, 0, 50);
        histograms[TString::Format("fsi%dHitMapPhi", i).Data()] = new TH1F(TString::Format("fsi%dHitMapPhi", i), TString::Format("FSI Layer %d; phi; "), 320, 0, TMath::Pi() * 2 + 0.1);
    }

    histograms["fstStudyPtRes"] = new TH1F("fstStudyPtRes", "FST; PtRes; ", 500, -5, 5);
    histograms["fstStudyPtResVsPt"] = new TH2F("fstStudyPtResVs", "FST; Pt; PtRes", 500, 0, 5, 500, -5, 5);

    histograms["fstStudyFastCovMatXY"] = new TH2F("fstStudyFastCovMatXY", "FST Fast Sim; #sigma_{X}; #sigma_{Y}", 500, 0, 5, 500, 0, 5);
    histograms["fstStudyCovMatXY"] = new TH2F("fstStudyCovMatXY", "FST; #sigma_{X}; #sigma_{Y}", 500, 0, 1, 500, 0, 1);
    histograms["fstStudyCovMatCompareX"] = new TH2F("fstStudyCovMatCompareX", "FST; #sigma_{X}; #sigma_{X} (Fast)", 500, 0, 1, 500, 0, 5);
    histograms["fstStudyCovMatCompareY"] = new TH2F("fstStudyCovMatCompareY", "FST; #sigma_{Y}; #sigma_{Y} (Fast)", 500, 0, 1, 500, 0, 5);

    return kStOK;
};

TMatrixDSym makeSiCovMat(TVector3 hit, jdb::XmlConfig &xfg) {
    // we can calculate the CovMat since we know the det info, but in future we should probably keep this info in the hit itself

    float r_size = xfg.get<float>("SiRasterizer:r", 3.0);
    float phi_size = xfg.get<float>("SiRasterizer:phi", 0.004);

    // measurements on a plane only need 2x2
    // for Si geom we need to convert from cylindrical to cartesian coords
    TMatrixDSym cm(2);
    TMatrixD T(2, 2);
    TMatrixD J(2, 2);
    const float x = hit.X();
    const float y = hit.Y();
    const float R = sqrt(x * x + y * y);
    const float cosphi = x / R;
    const float sinphi = y / R;
    const float sqrt12 = sqrt(12.);

    const float dr = r_size / sqrt12;
    const float nPhiDivs = 128 * 12; // may change with geom?
    const float dphi = (phi_size) / sqrt12;

    // Setup the Transposed and normal Jacobian transform matrix;
    // note, the si fast sim did this wrong
    // row col
    T(0, 0) = cosphi;
    T(0, 1) = -R * sinphi;
    T(1, 0) = sinphi;
    T(1, 1) = R * cosphi;

    J(0, 0) = cosphi;
    J(0, 1) = sinphi;
    J(1, 0) = -R * sinphi;
    J(1, 1) = R * cosphi;

    TMatrixD cmcyl(2, 2);
    cmcyl(0, 0) = dr * dr;
    cmcyl(1, 1) = dphi * dphi;

    TMatrixD r = T * cmcyl * J;

    const float sigmaX = sqrt(r(0, 0));
    const float sigmaY = sqrt(r(1, 1));

    cm(0, 0) = r(0, 0);
    cm(1, 1) = r(1, 1);
    cm(0, 1) = r(0, 1);
    cm(1, 0) = r(1, 0);

    LOG_F( INFO, "MY COVMAT = ( %f, %f, %f )", cm(0, 0), cm(0, 1), 0 );
    LOG_F( INFO, "MY COVMAT = ( %f, %f, %f )", cm(1, 0), cm(1, 1), 0 );
    LOG_F( INFO, "MY COVMAT = ( %f, %f, %f )", 0.0, 0.0, 0.0 );

    TMatrixDSym tamvoc(3);
    tamvoc( 0, 0 ) = cm(0, 0); tamvoc( 0, 1 ) = cm(0, 1); tamvoc( 0, 2 ) = 0.0;
    tamvoc( 1, 0 ) = cm(1, 0); tamvoc( 1, 1 ) = cm(1, 1); tamvoc( 1, 2 ) = 0.0;
    tamvoc( 2, 0 ) = 0.0;      tamvoc( 2, 1 ) = 0.0; tamvoc( 2, 2 )      = 0.01*0.01;


    return tamvoc;
}

void StgMaker::FstStudy(){
    LOG_SCOPE_FUNCTION(INFO);
    LOG_INFO << "StgMaker::FstStudy()   " << endm;

    jdb::XmlConfig _xmlconfig;
    _xmlconfig.loadFile(mConfigFile);
    // TrackFitter* tf = new TrackFitter(_xmlconfig);
    // tf->setup(false);

    StEvent *event = (StEvent *)this->GetDataSet("StEvent");
    if (0 == event) {
        cout << "No event found" << endl;
        return;
    }


    map<int, TLorentzVector> mclv;
    // load the McTracks
    St_g2t_track *g2t_track = (St_g2t_track *)GetDataSet("geant/g2t_track");
    if ( g2t_track == nullptr ) {
        LOG_INFO << "NO MC TRACKS" << endm;
        return;
    }
    LOG_INFO << "Checking " << g2t_track->GetNRows() << " Mc Tracks" << endm;
    for (int irow = 0; irow < g2t_track->GetNRows(); irow++) {
        g2t_track_st *track = (g2t_track_st *)g2t_track->At(irow);

        if (0 == track)
            continue;

        int track_id = track->id;
        float pt = track->pt;
        float eta = track->eta;
        float phi = atan2(track->p[1], track->p[0]); //track->phi;
        int q = track->charge;
        LOG_F(INFO, "Mc Track %d = ( %f, %f, %f ) start=%d", track_id, pt, eta, phi, track->start_vertex_p);

        TLorentzVector lv( track->p[0], track->p[1], track->p[2], track->e );
        mclv[ track_id ] = lv;


    } // loop on track (irow)


    St_g2t_fts_hit *g2t_stg_hits = (St_g2t_fts_hit *)GetDataSet("geant/g2t_stg_hit");

    StRnDHitCollection *rndCollection = event->rndHitCollection();
    if (0 == rndCollection) {
        cout << "No collection found" << endl;
    }
    const StSPtrVecRnDHit &hits = rndCollection->hits();
    LOG_INFO << "Found " << hits.size() << " FST hits from the fast sim" << endm;

    TMatrixDSym hitCov3(3);
    TVectorD sp(3);
    // not efficienct but oh well
    for ( size_t track_index = 1; track_index < g2t_track->GetNRows(); track_index++ ){

        if ( mclv.count( track_index ) == 0 ) continue;
        g2t_track_st *track = (g2t_track_st *)g2t_track->At(track_index-1);
        if ( track->start_vertex_p > 1 ) continue;

        int hitId = 0;
        vector<genfit::SpacepointMeasurement*> space_points;

        for (unsigned int fsthit_index = 0; fsthit_index < hits.size(); fsthit_index++) {
            StRnDHit *hit = hits[fsthit_index];
            if (0 == hit) {
                cout << "No hit found" << endl;
            }
            if ( hit->idTruth() != track_index ){
                // LOG_INFO << "Skipping hit from track : " << hit->idTruth() << " != " << track_index << endm;
                continue;
            }

            const StThreeVectorF pos = hit->position();
            LOG_F(INFO, "Fst Hit = (%f, %f, %f)", hit->position().x(), hit->position().y(), hit->position().z() );
            LOG_F(INFO, "Fst Hit True = (%f, %f, %f)", hit->double0(), hit->double1(), hit->double2() );

            // TMatrixDSym hitCov3(3);
            float cartsigXY = 0.01;
            hitCov3(0, 0) = cartsigXY * cartsigXY;
            hitCov3(1, 1) = cartsigXY * cartsigXY;
            hitCov3(2, 2) = 0.0087 * 0.0087;
            sp[0] = pos.x();
            sp[1] = pos.y();
            sp[2] = pos.z();
            LOG_F(INFO, "making space point");
            StMatrixF covmat = hit->covariantMatrix();
            // LOG_F( INFO, "COVMAT = (%f, %f, %f)", covmat[0][0], covmat[0][1], covmat[0][2] );
            // LOG_F( INFO, "COVMAT = (%f, %f, %f)", covmat[1][0], covmat[1][1], covmat[1][2] );
            // LOG_F( INFO, "COVMAT = (%f, %f, %f)", covmat[2][0], covmat[2][1], covmat[2][2] );

            // hitCov3(0,0) = covmat[0][0]; hitCov3(0,1) = covmat[0][1]; hitCov3(0,2) = covmat[0][2];
            // hitCov3(1,0) = covmat[1][0]; hitCov3(1,1) = covmat[1][1]; hitCov3(1,2) = covmat[1][2];
            // hitCov3(2,0) = covmat[2][0]; hitCov3(2,1) = covmat[2][1]; hitCov3(2,2) = covmat[2][2];

            hitCov3 = makeSiCovMat( TVector3( pos.x(), pos.y(), pos.z() ), xfg );
            histograms[ "fstStudyFastCovMatXY" ]->Fill( sqrt(covmat[0][0]), sqrt(covmat[1][1]) );
            histograms[ "fstStudyCovMatXY" ]->Fill( sqrt(hitCov3(0,0)), sqrt(hitCov3(1,1)) );
            histograms[ "fstStudyCovMatCompareX" ]->Fill( sqrt(hitCov3(0,0)), sqrt(covmat[0][0]) );
            histograms[ "fstStudyCovMatCompareY" ]->Fill( sqrt(hitCov3(1,1)), sqrt(covmat[1][1]) );

            try{
                space_points.push_back(new genfit::SpacepointMeasurement(sp, hitCov3, 0, ++hitId, nullptr));
            } catch ( genfit::Exception &e ){
                LOG_F(WARNING, "EXCEPTION");
            }

        }


        if ( g2t_stg_hits == nullptr ) continue;

        TVector3 seedPos;
        int nstg = 0;
        for (int i = 0; i < g2t_stg_hits->GetNRows(); i++) {
            // if ( nstg >= 4 ) continue;

            g2t_fts_hit_st *git = (g2t_fts_hit_st *)g2t_stg_hits->At(i);
            if (0 == git)
                continue; // geant hit
            int track_id = git->track_p;
            int volume_id = git->volume_id;
            int plane_id = (volume_id - 1) / 4;           // from 1 - 16. four chambers per station
            float x = git->x[0] + gRandom->Gaus(0, 0.01); // 100 micron blur according to approx sTGC reso
            float y = git->x[1] + gRandom->Gaus(0, 0.01); // 100 micron blur according to approx sTGC reso
            float z = git->x[2];

            if ( track_id != track_index ) continue;

            LOG_F(INFO, "STGC Hit: volume_id=%d, plane_id=%d, (%f, %f, %f), track_id=%d", volume_id, plane_id, x, y, z, track_id);
            
            // TMatrixDSym hitCov3(3);
            hitCov3(0, 0) = 0.01 * 0.01; hitCov3(0, 1) = 0; hitCov3(0, 2) = 0;
            hitCov3(1, 0) = 0; hitCov3(1, 1) = 0.01 * 0.01; hitCov3(1, 2) = 0;
            hitCov3(2, 0) = 0; hitCov3(2, 1) = 0; hitCov3(2, 2) = 0.005*0.005;
            sp[0] = x;
            sp[1] = y;
            sp[2] = z;
            seedPos.SetXYZ( x, y, z );
            
            space_points.push_back(new genfit::SpacepointMeasurement(sp, hitCov3, 0, ++hitId, nullptr));
            nstg++;
        }

        LOG_F( INFO, "We have %lu space points on the track, lets fit it", space_points.size() );
        if (space_points.size() < 7  ) continue;

        // TVector3 seedPos( sp[0], sp[1], sp[2] );
        // TVector3 seedMom( mclv[track_index].Px() + gRandom->Gaus( 0, 1 ), mclv[track_index].Py() + gRandom->Gaus( 0, 1 ), mclv[track_index].Pz() + gRandom->Gaus( 0, 1 ) );
        TVector3 seedMom(   mclv[track_index].Px(), 
                            mclv[track_index].Py(), 
                            mclv[track_index].Pz());

        TVector3 pfit = mForwardTracker->getTrackFitter()->fitSpacePoints( space_points, seedPos, seedMom );
        LOG_F(INFO,"mcp = %0.2f, %0.2f",mclv[track_index].Pt(), pfit.Perp() );
        histograms["fstStudyPtRes"]->Fill( (mclv[track_index].Pt() - pfit.Perp()) / mclv[track_index].Pt() );
        histograms["fstStudyPtResVsPt"]->Fill( mclv[track_index].Pt(), (mclv[track_index].Pt() - pfit.Perp())/mclv[track_index].Pt() );
    }

    return;
}

void StgMaker::loadStgcHits( std::map<int, shared_ptr<McTrack>> &mcTrackMap, std::map<int, std::vector<KiTrack::IHit *>> &hitMap ){
    /************************************************************/
    // STGC Hits
    St_g2t_fts_hit *g2t_stg_hits = (St_g2t_fts_hit *)GetDataSet("geant/g2t_stg_hit");

    // Add hits onto the hit loader (from rndHitCollection)
    int count = 0;

    // make the Covariance Matrix once and then reuse
    TMatrixDSym hitCov3(3);
    double sigXY = 0.01;
    hitCov3(0, 0) = sigXY * sigXY;
    hitCov3(1, 1) = sigXY * sigXY;
    hitCov3(2, 2) = 0.0; // unused since they are loaded as points on plane

    int nstg = 0;
    if (g2t_stg_hits == nullptr) {
        LOG_INFO << "g2t_stg_hits is null" << endm;
    } else {
        nstg = g2t_stg_hits->GetNRows();
    }

    {
        LOG_SCOPE_F(INFO, "Loading sTGC hits");

        LOG_INFO << "# stg hits= " << nstg << endm;
        this->histograms["nHitsSTGC"]->Fill(nstg);
        this->mlt_n = 0;
        for (int i = 0; i < nstg; i++) {

            g2t_fts_hit_st *git = (g2t_fts_hit_st *)g2t_stg_hits->At(i);
            if (0 == git)
                continue; // geant hit
            int track_id = git->track_p;
            int volume_id = git->volume_id;
            int plane_id = (volume_id - 1) / 4;           // from 1 - 16. four chambers per station
            float x = git->x[0] + gRandom->Gaus(0, 0.01); // 100 micron blur according to approx sTGC reso
            float y = git->x[1] + gRandom->Gaus(0, 0.01); // 100 micron blur according to approx sTGC reso
            float z = git->x[2];

            if (mGenTree) {
                mlt_x[mlt_n] = x;
                mlt_y[mlt_n] = y;
                mlt_z[mlt_n] = z;
                mlt_tid[mlt_n] = track_id;
                mlt_vid[mlt_n] = plane_id;
                mlt_hpt[mlt_n] = mcTrackMap[track_id]->_pt;
                mlt_hsv[mlt_n] = mcTrackMap[track_id]->_start_vertex;
                mlt_n++;
            }

            LOG_F(INFO, "STGC Hit: volume_id=%d, plane_id=%d, (%f, %f, %f), track_id=%d", volume_id, plane_id, x, y, z, track_id);
            this->histograms["stgc_volume_id"]->Fill(volume_id);

            if (plane_id < 4 && plane_id >= 0) {
                this->histograms[TString::Format("stgc%dHitMap", plane_id).Data()]->Fill(x, y);
            } else {
                LOG_F(ERROR, "Out of bounds STGC plane_id!");
                continue;
            }

            FwdHit *hit = new FwdHit(count++, x, y, z, -plane_id, track_id, hitCov3, mcTrackMap[track_id]);

            // Add the hit to the hit map
            hitMap[hit->getSector()].push_back(hit);

            // Add hit pointer to the track
            if (mcTrackMap[track_id])
                mcTrackMap[track_id]->addHit(hit);
        }
    } // Loading sTGC hits
} // loadStgcHits

void StgMaker::loadFstHits( std::map<int, shared_ptr<McTrack>> &mcTrackMap, std::map<int, std::vector<KiTrack::IHit *>> &hitMap, int count ){
    LOG_SCOPE_FUNCTION( INFO );

    // Get the StEvent handle to see if the rndCollection is available
    StEvent *event = (StEvent *)this->GetDataSet("StEvent");
    StRnDHitCollection *rndCollection = nullptr;
    if (nullptr != event) {
        rndCollection = event->rndHitCollection();
    }
    bool siRasterizer = xfg.get<bool>( "SiRasterizer:active", false );
    LOG_F( INFO, "siRasterizer active=%d, r=%f", (int)(siRasterizer), xfg.get<float>( "SiRasterizer:r") );
    if ( siRasterizer || rndCollection == nullptr ){
        LOG_F( INFO, "Loading hits from GEANT with SiRasterizer" );
        loadFstHitsFromGEANT( mcTrackMap, hitMap, count );
    } else {
        loadFstHitsFromStEvent( mcTrackMap, hitMap, count );
    }
} // loadFstHits

void StgMaker::loadFstHitsFromStEvent( std::map<int, shared_ptr<McTrack>> &mcTrackMap, std::map<int, std::vector<KiTrack::IHit *>> &hitMap, int count ){
    LOG_SCOPE_FUNCTION( INFO );

    // Get the StEvent handle
    StEvent *event = (StEvent *)this->GetDataSet("StEvent");
    if (0 == event) {
        LOG_F( ERROR, "No StEvent found" );
        return;
    }

    StRnDHitCollection *rndCollection = event->rndHitCollection();
    if (0 == rndCollection) {
        LOG_F( ERROR, "No StRnDHitCollection found" );
    }

    const StSPtrVecRnDHit &hits = rndCollection->hits();
    LOG_F( INFO, "Found %lu FST hits in StEvent collection", hits.size() );

    // we will reuse this to hold the cov mat
    TMatrixDSym hitCov3(3);
    

    for (unsigned int fsthit_index = 0; fsthit_index < hits.size(); fsthit_index++) {
        StRnDHit *hit = hits[fsthit_index];
        if (0 == hit) {
            LOG_F( INFO, "NULL hit at index %lu", fsthit_index );
        }

        const StThreeVectorF pos = hit->position();
        LOG_F(INFO, "Fst Hit = (%f, %f, %f)", hit->position().x(), hit->position().y(), hit->position().z() );
        LOG_F(INFO, "Fst Hit True = (%f, %f, %f)", hit->double0(), hit->double1(), hit->double2() );

        StMatrixF covmat = hit->covariantMatrix();

        // copy covariance matrix element by element from StMatrixF
        hitCov3(0,0) = covmat[0][0]; hitCov3(0,1) = covmat[0][1]; hitCov3(0,2) = covmat[0][2];
        hitCov3(1,0) = covmat[1][0]; hitCov3(1,1) = covmat[1][1]; hitCov3(1,2) = covmat[1][2];
        hitCov3(2,0) = covmat[2][0]; hitCov3(2,1) = covmat[2][1]; hitCov3(2,2) = covmat[2][2];

        FwdHit *fhit = new FwdHit(count++, hit->position().x(), hit->position().y(), hit->position().z(), hit->layer(), -1, hitCov3, nullptr);

        // Add the hit to the hit map
        hitMap[fhit->getSector()].push_back(fhit);

    }
} //loadFstHitsFromStEvent

void StgMaker::loadFstHitsFromGEANT( std::map<int, shared_ptr<McTrack>> &mcTrackMap, std::map<int, std::vector<KiTrack::IHit *>> &hitMap, int count ){
    LOG_SCOPE_FUNCTION(INFO);
    /************************************************************/
    // FSI Hits
    int nfsi = 0;
    St_g2t_fts_hit *g2t_fsi_hits = (St_g2t_fts_hit *)GetDataSet("geant/g2t_fsi_hit");

    if (g2t_fsi_hits == nullptr) {
        LOG_INFO << "g2t_fsi_hits is null" << endm;
    } else {
        nfsi = g2t_fsi_hits->GetNRows();
    }

    // reuse this to store cov mat
    TMatrixDSym hitCov3(3);
    
    this->histograms["nHitsFSI"]->Fill(nfsi);
    LOG_INFO << "# fsi hits = " << nfsi << endm;

    for (int i = 0; i < nfsi; i++) {

        g2t_fts_hit_st *git = (g2t_fts_hit_st *)g2t_fsi_hits->At(i);
        
        if (0 == git)
            continue; // geant hit
        
        int track_id = git->track_p;
        int volume_id = git->volume_id;  // 4, 5, 6
        int d = volume_id / 1000;        // disk id
        int w = (volume_id % 1000) / 10; // wedge id
        int s = volume_id % 10;          // sensor id
        LOG_F(INFO, "volume_id=%d, disk=%d, wedge=%d, sensor=%d", volume_id, d, w, s);
        int plane_id = d - 4;
        float x = git->x[0];
        float y = git->x[1];
        float z = git->x[2];

        if (mSiRasterizer->active()) {
            TVector3 rastered = mSiRasterizer->raster(TVector3(git->x[0], git->x[1], git->x[2]));
            this->histograms["fsiHitDeltaR"]->Fill(sqrt(x * x + y * y) - rastered.Perp());
            this->histograms["fsiHitDeltaPhi"]->Fill(atan2(y, x) - rastered.Phi());
            x = rastered.X();
            y = rastered.Y();
        }

        LOG_F(INFO, "FSI Hit: volume_id=%d, plane_id=%d, (%f, %f, %f), track_id=%d", volume_id, plane_id, x, y, z, track_id);
        this->histograms["fsi_volume_id"]->Fill(d);

        if (plane_id < 3 && plane_id >= 0) {
            this->histograms[TString::Format("fsi%dHitMap", plane_id).Data()]->Fill(x, y);
            this->histograms[TString::Format("fsi%dHitMapR", plane_id).Data()]->Fill(sqrt(x * x + y * y));
            this->histograms[TString::Format("fsi%dHitMapPhi", plane_id).Data()]->Fill(atan2(y, x) + TMath::Pi());
        } else {
            LOG_F(ERROR, "Out of bounds FSI plane_id!");
            continue;
        }

        hitCov3 = makeSiCovMat( TVector3( x, y, z ), xfg );
        FwdHit *hit = new FwdHit(count++, x, y, z, d, track_id, hitCov3, mcTrackMap[track_id]);

        // Add the hit to the hit map
        hitMap[hit->getSector()].push_back(hit);
    }
} // loadFstHitsFromGEANT

void StgMaker::loadMcTracks( std::map<int, std::shared_ptr<McTrack>> &mcTrackMap ){
    LOG_SCOPE_FUNCTION( INFO );
    // Get geant tracks
    St_g2t_track *g2t_track = (St_g2t_track *)GetDataSet("geant/g2t_track");

    if (!g2t_track)
        return;

    mlt_nt = 1;
    LOG_INFO << "# mc tracks = " << g2t_track->GetNRows() << endm;
    this->histograms["nMcTracks"]->Fill(g2t_track->GetNRows());

    if (g2t_track) {
        LOG_SCOPE_F(INFO, "MC Tracks");
        for (int irow = 0; irow < g2t_track->GetNRows(); irow++) {
            g2t_track_st *track = (g2t_track_st *)g2t_track->At(irow);

            if (0 == track)
                continue;

            int track_id = track->id;
            float pt2 = track->p[0] * track->p[0] + track->p[1] * track->p[1];
            float pt = sqrt(pt2);
            float eta = track->eta;
            float phi = atan2(track->p[1], track->p[0]); //track->phi;
            int q = track->charge;
            LOG_F(INFO, "Mc Track %d = ( %f, %f, %f )", track_id, pt, eta, phi);

            if (0 == mcTrackMap[track_id]) // should always happen
                mcTrackMap[track_id] = shared_ptr<McTrack>(new McTrack(pt, eta, phi, q, track->start_vertex_p));
            
            if (mGenTree) {
                LOG_F(INFO, "mlt_nt = %d == track_id = %d, is_shower = %d, start_vtx = %d", mlt_nt, track_id, track->is_shower, track->start_vertex_p);
                mlt_pt[mlt_nt] = pt;
                mlt_eta[mlt_nt] = eta;
                mlt_phi[mlt_nt] = phi;
                mlt_nt++;
            }

        } // loop on track (irow)
    } // if g2t_track
} // loadMcTracks


//________________________________________________________________________
int StgMaker::Make() {
    LOG_INFO << "StgMaker::Make()   " << endm;

    jdb::XmlConfig _xmlconfig;
    _xmlconfig.loadFile(mConfigFile);

    long long itStart = loguru::now_ns();
    
    std::map<int, shared_ptr<McTrack>> &mcTrackMap = mForwardHitLoader->_mctracks;
    std::map<int, std::vector<KiTrack::IHit *>> &hitMap = mForwardHitLoader->_hits;
    std::map<int, std::vector<KiTrack::IHit *>> &fsiHitMap = mForwardHitLoader->_fsi_hits;

    loadMcTracks( mcTrackMap );
    loadStgcHits( mcTrackMap, hitMap );
    loadFstHits( mcTrackMap, fsiHitMap );

    LOG_INFO << "mForwardTracker -> doEvent()" << endm;

    // Process single event
    mForwardTracker->doEvent();

    if (mGenTree) {
        LOG_SCOPE_F(INFO, "Saving Criteria into TTree");
        if (mForwardTracker->getSaveCriteriaValues()) {
            for (auto crit : mForwardTracker->getTwoHitCriteria()) {
                string name = crit->getName();
                mlt_crits[name].clear();
                mlt_crit_track_ids[name].clear();
                // copy by value so ROOT doesnt get lost (uses pointer to vector)
                for (float v : mForwardTracker->getCriteriaValues(name)) {
                    mlt_crits[name].push_back(v);
                }
                for (int v : mForwardTracker->getCriteriaTrackIds(name)) {
                    mlt_crit_track_ids[name].push_back(v);
                }
            }

            // three hit criteria
            for (auto crit : mForwardTracker->getThreeHitCriteria()) {
                string name = crit->getName();
                mlt_crits[name].clear();
                mlt_crit_track_ids[name].clear();
                // copy by value so ROOT doesnt get lost (uses pointer to vector)
                for (float v : mForwardTracker->getCriteriaValues(name)) {
                    mlt_crits[name].push_back(v);
                }
                for (int v : mForwardTracker->getCriteriaTrackIds(name)) {
                    mlt_crit_track_ids[name].push_back(v);
                }
            }
        }

        mlTree->Fill();
    } // if mGenTree

    LOG_INFO << "Event took " << (loguru::now_ns() - itStart) * 1e-6 << " ms" << endm;

    return kStOk;
    StEvent *event = static_cast<StEvent *>(GetInputDS("StEvent"));

    if (0 == event) {
        LOG_INFO << "No event, punt on forward tracking." << endm;
        return kStOk;
    }

    // Now fill StEvent
    FillEvent();

    // Now loop over the tracks and do printout
    int nnodes = event->trackNodes().size();

    for (int i = 0; i < nnodes; i++) {

        const StTrackNode *node = event->trackNodes()[i];
        StGlobalTrack *track = (StGlobalTrack *)node->track(global);
        StTrackGeometry *geometry = track->geometry();

        StThreeVectorF origin = geometry->origin();
        StThreeVectorF momentum = geometry->momentum();

        LOG_INFO << "-------------------------------------------------------------------------------" << endm;
        LOG_INFO << "Track # " << i << endm;
        LOG_INFO << "inner: Track origin: " << origin << " momentum: " << momentum << " pt=" << momentum.perp() << " eta=" << momentum.pseudoRapidity() << endm;

        StDcaGeometry *dca = track->dcaGeometry();
        origin = dca->origin();
        momentum = dca->momentum();
        LOG_INFO << "d c a: Track origin: " << origin << " momentum: " << momentum << " pt=" << momentum.perp() << " eta=" << momentum.pseudoRapidity() << endm;

        int idtruth = track->idTruth();
        LOG_INFO << " idtruth = " << idtruth << endm;
        auto mctrack = mcTrackMap[idtruth];

        if (mctrack)
            LOG_INFO << "truth: pt=" << mctrack->_pt << " eta=" << mctrack->_eta << " phi=" << mctrack->_phi << " q=" << mctrack->_q << endm;
    }

    return kStOK;
}
//________________________________________________________________________
void StgMaker::Clear(const Option_t *opts) {
    mForwardHitLoader->clear();
}
//________________________________________________________________________
void StgMaker::FillEvent() {

    StEvent *event = static_cast<StEvent *>(GetInputDS("StEvent"));
    assert(event); // we warned ya

    LOG_INFO << "Filling StEvent w/ results from genfit tracker" << endm;

    // Track seeds
    const auto &seed_tracks = mForwardTracker->getRecoTracks();
    // Reconstructed globals
    const auto &glob_tracks = mForwardTracker->globalTracks();

    // Clear up somethings... (but does this interfere w/ Sti and/or Stv?)
    StEventHelper::Remove(event, "StSPtrVecTrackNode");
    StEventHelper::Remove(event, "StSPtrVecPrimaryVertex");

    LOG_INFO << "  number of tracks      = " << glob_tracks.size() << endm;
    LOG_INFO << "  number of track seeds = " << seed_tracks.size() << endm;

    // StiStEventFiller fills track nodes and detector infos by reference... there
    // has got to be a cleaner way to do this, but for now follow along.
    auto &trackNodes = event->trackNodes();
    auto &trackDetectorInfos = event->trackDetectorInfo();

    int track_count_total = 0;
    int track_count_accept = 0;

    for (auto *track : mForwardTracker->globalTracks()) {

        // Get the track seed
        const auto &seed = seed_tracks[track_count_total];

        // Increment total track count
        track_count_total++;

        // Check to see if the track passes cuts (it should, for now)
        if (0 == accept(track))
            continue;

        track_count_accept++;

        // Create a detector info object to be filled
        StTrackDetectorInfo *detectorInfo = new StTrackDetectorInfo;
        FillDetectorInfo(detectorInfo, track, true);

        // Create a new track node (on which we hang a global and, maybe, primary track)
        StTrackNode *trackNode = new StTrackNode;

        // This is our global track, to be filled from the genfit::Track object "track"
        StGlobalTrack *globalTrack = new StGlobalTrack;

        // Fill the track with the good stuff
        FillTrack(globalTrack, track, seed, detectorInfo);
        trackNode->addTrack(globalTrack);

        // On successful fill (and I don't see why we wouldn't be) add detector info to the list
        trackDetectorInfos.push_back(detectorInfo);

        trackNodes.push_back(trackNode);

        // // Set relationships w/ tracker object and MC truth
        // // globalTrack->setKey( key );
        // // globalTrack->setIdTruth( idtruth, qatruth ); // StTrack is dominant contributor model

        // // Add the track to its track node
        // trackNode->addTrack( globalTrack );
        // trackNodes.push_back( trackNode );

        // NOTE: could we qcall here mForwardTracker->fitTrack( seed, vertex ) ?

    } // end of loop over tracks

    LOG_INFO << "  number accepted = " << track_count_accept << endm;
}
//________________________________________________________________________
void StgMaker::FillTrack(StTrack *otrack, genfit::Track *itrack, const Seed_t &iseed, StTrackDetectorInfo *info) {
    const double z_fst[] = {93.3, 140.0, 186.6};
    const double z_stgc[] = {280.9, 303.7, 326.6, 349.4};

    // otrack == output track
    // itrack == input track (genfit)

    otrack->setEncodedMethod(kUndefinedFitterId);

    // Track length and TOF between first and last point on the track
    // TODO: is this same definition used in StEvent?
    double track_len = itrack->getTrackLen();
    //  double track_tof = itrack->getTrackTOF();
    otrack->setLength(track_len);

    // Get the so called track seed quality... the number of hits in the seed
    int seed_qual = iseed.size();
    otrack->setSeedQuality(seed_qual);

    // Set number of possible points in each detector
    // TODO: calcuate the number of possible points in each detector, for now set = 4
    otrack->setNumberOfPossiblePoints(4, kUnknownId);

    // Fill the inner and outer geometries of the track.  For now,
    // always propagate the track to the first layer of the silicon
    // to fill the inner geometry.
    //
    // TODO: We may need to extend our "geometry" classes for RK parameters
    FillTrackGeometry(otrack, itrack, z_fst[0], kInnerGeometry);
    FillTrackGeometry(otrack, itrack, z_stgc[3], kOuterGeometry);

    // Next fill the fit traits
    FillTrackFitTraits(otrack, itrack);

    // Set detector info
    otrack->setDetectorInfo(info);

    // NOTE:  StStiEventFiller calls StuFixTopoMap here...

    // Fill the track flags
    FillTrackFlags(otrack, itrack);

    // If the track is a global track, fill the DCA geometry
    if (global == otrack->type())
        FillTrackDcaGeometry(otrack, itrack);

    //covM[k++] = M(0,5); covM[k++] = M(1,5); covM[k++] = M(2,5); covM[k++] = M(3,5); covM[k++] = M(4,5); covM[k++] = M(5,5);
}
//________________________________________________________________________
void StgMaker::FillTrackFlags(StTrack *otrack, genfit::Track *itrack) {

    int flag = 0;
    // StiStEventFiller::setFlag does two things.  1) it sets the track flags, indicating
    // which detectors have participated in the track.  It is a four digit value encoded
    // as follows (from StTrack.h):

    /* --------------------------------------------------------------------------
	 *  The track flag (mFlag accessed via flag() method) definitions with ITTF
	 *  (flag definition in EGR era can be found at
	 *   http://www.star.bnl.gov/STAR/html/all_l/html/dst_track_flags.html)
	 *
	 *  mFlag= zxyy, where  z = 1 for pile up track in TPC (otherwise 0)
	 *                      x indicates the detectors included in the fit and
	 *                     yy indicates the status of the fit.
	 *  Positive mFlag values are good fits, negative values are bad fits.
	 *
	 *  The first digit indicates which detectors were used in the refit:
	 *
	 *      x=1 -> TPC only
	 *      x=3 -> TPC       + primary vertex
	 *      x=5 -> SVT + TPC
	 *      x=6 -> SVT + TPC + primary vertex
	 *      x=7 -> FTPC only
	 *      x=8 -> FTPC      + primary
	 *      x=9 -> TPC beam background tracks
	 *
	 *  The last two digits indicate the status of the refit:
	 *       = +x01 -> good track
	 *
	 *      = -x01 -> Bad fit, outlier removal eliminated too many points
	 *      = -x02 -> Bad fit, not enough points to fit
	 *      = -x03 -> Bad fit, too many fit iterations
	 *      = -x04 -> Bad Fit, too many outlier removal iterations
	 *      = -x06 -> Bad fit, outlier could not be identified
	 *      = -x10 -> Bad fit, not enough points to start
	 *
	 *      = +x11 -> Short track pointing to EEMC */

    // NOTE: First digit will be used as follows for forward tracks
    //
    // x = 5 sTGC only
    // x = 6 sTGC + primary vertex
    // x = 7 sTGC + forward silicon
    // x = 8 sTGC + forward silicon + primary vertex

    if (global == otrack->type())
        flag = 501;
    else if (primary == otrack->type())
        flag = 601;

    // TODO: detect presence of silicon hits and add appropriately to the flag

    // As for "bad" fits, I believe GenFit does not propagate fit information for
    // failed fits.  (???).  So we will not publish bad track flags.
}
//________________________________________________________________________
void StgMaker::FillTrackMatches(StTrack *otrack, genfit::Track *itrack) {

    // TODO:

    // At midrapidity, we extend the track to the fast detectors and check to see whether
    // the track matches an active element or not.  The fast detectors are the barrel time-
    // of-flight, the barrel EM calorimeter and teh endcap EM calorimeter.

    // We will be interested in matching FTS tracks to the following subsystems:
    // 1) The event plane detector
    // 2) Forward EM cal
    // 3) Forward Hadronic cal

    // We could adopt the following scheme to save the track fit information in a way that
    // can be accessed later, without modification to the StEvent data model...

    // Save the state of the fit (mapped to a helix) at the first silicon layer as the inner geometry.
    // Save the state of the fit (mapped to a helix) at the event plane detector as the outer geometry.
    // Save the state of the fit (mapped to a helix) at the front of the EM cal as the "Ext" geometry
    // ... helix would have no curvature at that point and would be a straight line, as there is no b field.
    // ... can easily get to the HCAL from there...
}
//________________________________________________________________________
void StgMaker::FillTrackFitTraits(StTrack *otrack, genfit::Track *itrack) {

    const double z_fst[] = {93.3, 140.0, 186.6};
    const double z_stgc[] = {280.9, 303.7, 326.6, 349.4};

    unsigned short g3id_pid_hypothesis = 6; // TODO: do not hard code this

    // Set the chi2 of the fit.  The second element in the array is the incremental
    // chi2 for adding the vertex to the primary track.
    float chi2[] = {0, -999};
    const auto *fit_status = itrack->getFitStatus();

    if (0 == fit_status) {
        LOG_WARN << "genfit track with no fit status" << endm;
        return;
    }

    chi2[0] = fit_status->getChi2();
    int ndf = fit_status->getNdf();

    chi2[0] /= ndf; // TODO: Check if this is right

    // ... odd that we make this determination based on the output track's type ...
    if (primary == otrack->type()) {
        // TODO: chi2[1] should hold the incremental chi2 of adding the vertex for the primary track
        //       is this available from genfit?
    }

    // Covariance matrix is next.  This one should be fun.  StEvent assumes the helix
    // model, but we have fit to the Runga Kutta track model.  The covariance matrix
    // is different.  So... TODO:  Do we need to specify covM for the equivalent helix?
    float covM[15] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    // Obtain fitted state so we can grab the covariance matrix
    genfit::MeasuredStateOnPlane state = itrack->getFittedState(0);

    // For global tracks, we are evaluating the fit at the first silicon plane.
    // Extrapolate the fit to this point so we can extract the covariance matrix
    // there.  For primary track, point 0 should correspond to the vertex.
    //
    // TODO: verify first point on primary tracks is the vertex.

    // ... fit traits will be evaluated at first point on track for now ...

    // if ( global == otrack->type() ) {

    //   // Obtain the cardinal representation
    //   genfit::AbsTrackRep* cardinal =  itrack->getCardinalRep();

    //   // We really don't want the overhead in the TVector3 ctor/dtor here
    //   static TVector3 xhat(1,0,0), yhat(0,1,0), Z(0,0,0);

    //   // Assign the z position
    //   Z[2] = z_fst[0];

    //   // This is the plane for which we are evaluating the fit
    //   const auto detectorPlane = genfit::SharedPlanePtr( new genfit::DetPlane(Z, xhat, yhat) );

    //   // Update the state to the given plane
    //   cardinal->extrapolateToPlane( state, detectorPlane, false, true );

    // }

    // Grab the covariance matrix
    const auto &M = state.getCov();

    // TODO: This is where we would do the math and transform from the Runga Kutta basis
    //       to the helix basis... but do we need to?

    int k = 0;
    covM[k++] = M(0, 0);
    covM[k++] = M(0, 1);
    covM[k++] = M(1, 1);
    covM[k++] = M(0, 2);
    covM[k++] = M(1, 2);
    covM[k++] = M(2, 2);
    covM[k++] = M(0, 3);
    covM[k++] = M(1, 3);
    covM[k++] = M(2, 3);
    covM[k++] = M(3, 3);
    covM[k++] = M(0, 4);
    covM[k++] = M(1, 4);
    covM[k++] = M(2, 4);
    covM[k++] = M(3, 4);
    covM[k++] = M(4, 4);

    StTrackFitTraits fit_traits(g3id_pid_hypothesis, 0, chi2, covM);

    // Get number of hits in all detectors
    int nhits[kMaxDetectorId] = {};

    for (const auto *point : itrack->getPoints()) {

        const auto *measurement = point->getRawMeasurement();
        int detId = measurement->getDetId();
        nhits[detId]++;
    }

    for (int i = 0; i < kMaxDetectorId; i++) {
        if (0 == nhits[i])
            continue; // not sure why, but Sti skips setting zero hits

        fit_traits.setNumberOfFitPoints((unsigned char)nhits[i], (StDetectorId)i);
    }

    if (primary == otrack->type()) {
        fit_traits.setPrimaryVertexUsedInFit(true);
    }

    otrack->setFitTraits(fit_traits);
}
//________________________________________________________________________
void StgMaker::FillTrackGeometry(StTrack *otrack, genfit::Track *itrack, double zplane, int io) {

    int ipoint = 0;

    if (io == kInnerGeometry)
        ipoint = 0; // hardcoded to sTGC only for now
    else
        ipoint = 3;

    // Obtain fitted state
    genfit::MeasuredStateOnPlane measuredState = itrack->getFittedState(ipoint);

    // Obtain the cardinal representation
    genfit::AbsTrackRep *cardinal = itrack->getCardinalRep();

    // We really don't want the overhead in the TVector3 ctor/dtor here
    static TVector3 xhat(1, 0, 0), yhat(0, 1, 0), Z(0, 0, 0);

    // Assign the z position
    Z[2] = zplane;

    // This is the plane for which we are evaluating the fit
    const auto detectorPlane = genfit::SharedPlanePtr(new genfit::DetPlane(Z, xhat, yhat));

    // Update the state to the given plane
    try {
        cardinal->extrapolateToPlane(measuredState, detectorPlane, false, true);
    } catch (genfit::Exception &e) {
        std::cerr << e.what() << std::endl;
        std::cerr << "Extraploation to inner/outer geometry point failed" << endl;
        //    assert(0);
        // extrapolation failed
        return;
    }

    //  measuredState.Print();

    static StThreeVector<double> momentum;
    static StThreeVector<double> origin;

    static TVector3 pos;
    static TVector3 mom;
    static TMatrixDSym cov;

    measuredState.getPosMomCov(pos, mom, cov);

    for (int i = 0; i < 3; i++)
        momentum[i] = mom[i];

    for (int i = 0; i < 3; i++)
        origin[i] = pos[i];

    double charge = measuredState.getCharge();

    // Get magnetic field
    double X[] = {pos[0], pos[1], pos[2]};
    double B[] = {0, 0, 0};
    StarMagField::Instance()->Field(X, B);

    // This is really an approximation, should be good enough for the inner
    // geometry (in the Silicon) but terrible in the outer geometry ( sTGC)
    double Bz = B[2];

    // Temporary helix to get the helix parameters
    StPhysicalHelix helix(momentum, origin, Bz, charge);
    // StiStEventFiller has this as |curv|.
    double curv = TMath::Abs(helix.curvature());
    double h = -TMath::Sign(charge * Bz, 1.0); // helicity

    if (charge == 0)
        h = 1;

    //
    // From StHelix::helix()
    //
    // phase = mPsi - h*pi/2
    // so...
    // psi = phase + h*pi/2
    //
    double psi = helix.phase() + h * TMath::Pi() / 2;
    double dip = helix.dipAngle();
    short q = charge;
    assert(q == 1 || q == -1 || q == 0);

    // Create the track geometry
    StTrackGeometry *geometry = new StHelixModel(q, psi, curv, dip, origin, momentum, h);

    // TODO: check helix parameters... geometry->helix() should return an StPhysicalHelix...
    //       double check that we converted everthing correctly.

    if (kInnerGeometry == io)
        otrack->setGeometry(geometry);
    else
        otrack->setOuterGeometry(geometry);
}
//________________________________________________________________________
void StgMaker::FillTrackDcaGeometry(StTrack *otrack_, genfit::Track *itrack) {

    // Recast to global track
    StGlobalTrack *otrack = dynamic_cast<StGlobalTrack *>(otrack_);

    if (0 == otrack)
        return;

    // Obtain fitted state from genfit track
    genfit::MeasuredStateOnPlane measuredState = itrack->getFittedState(1);

    // Obtain the cardinal representation
    genfit::AbsTrackRep *cardinal = itrack->getCardinalRep();

    const TVector3 vertex(0., 0., 0.); // TODO get actual primary vertex
    const TVector3 direct(0., 0., 1.); // TODO get actual beamline slope

    // Extrapolate the measured state to the DCA of the beamline
    try {
        cardinal->extrapolateToLine(measuredState, vertex, direct, false, true);
    } catch (genfit::Exception &e) {
        std::cerr << e.what() << std::endl;
        std::cerr << "Extrapolation to beamline (DCA) failed." << std::endl;
        assert(0);
        return;
    }

    static StThreeVector<double> momentum;
    static StThreeVector<double> origin;

    //
    // These lines obtain the position, momentum and covariance matrix for the fit
    //

    static TVector3 pos;
    static TVector3 mom;

    measuredState.getPosMom(pos, mom);

    for (int i = 0; i < 3; i++)
        momentum[i] = mom[i];

    for (int i = 0; i < 3; i++)
        origin[i] = pos[i];

    double charge = measuredState.getCharge();

    static TVectorD state;
    static TMatrixDSym cov;

    //
    // Should be the 5D state and covariance matrix
    //  https://arxiv.org/pdf/1902.04405.pdf
    //  state = { q/p, u', v', u, v }, where
    //  q/p is charge over momentum
    //  u,  v correspond to x, y (I believe)
    //  u', v' are the direction cosines with respect to the plane
    //  ... presume that
    //      u' = cos(thetaX)
    //      v' = cos(thetaY)
    //
    state = measuredState.getState();
    cov = measuredState.getCov();

    // Below is one way to convert the parameters to a helix, using the
    // StPhysicalHelix class

#if 1
    double eta = momentum.pseudoRapidity();
    double pt = momentum.perp();
    double ptinv = (pt != 0) ? 1.0 / pt : std::numeric_limits<double>::max();

    // Get magnetic field
    double X[] = {pos[0], pos[1], pos[2]};
    double B[] = {0, 0, 0};
    StarMagField::Instance()->Field(X, B);

    // This is really an approximation, should be good enough for the inner
    // geometry (in the Silicon) but terrible in the outer geometry ( sTGC)
    double Bz = B[2];

    // Temporary helix to get the helix parameters
    StPhysicalHelix helix(momentum, origin, Bz, charge);

    // StiStEventFiller has this as |curv|.
    double curv = TMath::Abs(helix.curvature());
    double h = -TMath::Sign(charge * Bz, 1.0); // helicity

    if (charge == 0)
        h = 1;

    //
    // From StHelix::helix()
    //
    // phase = mPsi - h*pi/2
    // so...
    // psi = phase + h*pi/2
    //
    double psi = helix.phase() + h * TMath::Pi() / 2;
    double dip = helix.dipAngle();
    double tanl = TMath::Tan(dip); // TODO: check this
    short q = charge;
    assert(q == 1 || q == -1 || q == 0);

    /* These are the seven parameters defined in DCA geometry...

		/// signed impact parameter; Signed in such a way that (in Sti local coords????)
		///     x =  -impact*sin(Psi)
		///     y =   impact*cos(Psi)
		Float_t  mImp;
		///  Z-coordinate of this track (reference plane)
		Float_t  mZ;
		///  Psi angle of the track
		Float_t  mPsi;
		/// signed invert pt [sign = sign(-qB)]
		Float_t  mPti;
		/// tangent of the track momentum dip angle
		Float_t  mTan;
		/// signed curvature
		Float_t  mCurv;

	*/

    // TODO: is this right?
    double mImp = origin.perp();
    double mZ = origin[2];
    double mPsi = psi;
    double mPti = ptinv;
    double mTan = tanl;
    double mCurv = curv;

    double p[] = {mImp, mZ, mPsi, mPti, mTan, mCurv};

    // TODO: fill in errors... (do this numerically?)
    double e[15] = {};

    StDcaGeometry *dca = new StDcaGeometry;
    otrack->setDcaGeometry(dca);
    dca->set(p, e);

#endif
}
// //________________________________________________________________________
void StgMaker::FillDetectorInfo(StTrackDetectorInfo *info, genfit::Track *track, bool increment) {

    //   // here is where we would fill in
    //   // 1) total number of hits
    //   // 2) number of sTGC hits
    //   // 3) number of silicon hits
    //   // 4) an StHit for each hit fit to the track
    //   // 5) The position of the first and last hits on the track

    int ntotal = track->getNumPoints(); // vs getNumPointsWithMeasurement() ?
    int nstgc = ntotal;

    float zmin = 9E9;
    float zmax = -9E9;

    StThreeVectorF firstPoint(0, 0, 9E9);
    StThreeVectorF lastPoint(0, 0, -9E9);

    int count = 0;

    for (const auto *point : track->getPoints()) {

        const auto *measurement = point->getRawMeasurement();
        const TVectorD &xyz = measurement->getRawHitCoords();
        float x = xyz[0];
        float y = xyz[1];
        float z = 0; // We get this from the detector plane...

        // Get fitter info for the cardinal representation
        const auto *fitinfo = point->getFitterInfo();

        const auto &plane = fitinfo->getPlane();
        TVector3 normal = plane->getNormal();
        const TVector3 &origin = plane->getO();

        z = origin[2];

        if (z > lastPoint[2]) {
            lastPoint.setX(x);
            lastPoint.setY(y);
            lastPoint.setZ(z);
        }

        if (z < firstPoint[2]) {
            firstPoint.setX(x);
            firstPoint.setY(y);
            firstPoint.setZ(z);
        }

        int detId = measurement->getDetId();
        int hitId = measurement->getHitId();

        ++count;

        //TODO: Convert (or access) StHit and add to the track detector info
    }

    assert(count);

    info->setFirstPoint(firstPoint);
    info->setLastPoint(lastPoint);
    info->setNumberOfPoints(ntotal, kUnknownId); // TODO: Assign
}
//________________________________________________________________________
