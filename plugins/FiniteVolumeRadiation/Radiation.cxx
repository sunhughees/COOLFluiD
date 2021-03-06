#include <fstream>
#include <iostream>

#include "Common/PE.hh"
#include "Common/BadValueException.hh"
#include "Common/CFPrintContainer.hh"

#include "MathTools/MathConsts.hh"

#include "Environment/ObjectProvider.hh"
#include "Environment/CFEnv.hh"
#include "Environment/FileHandlerOutput.hh"
#include "Environment/FileHandlerInput.hh"
#include "Environment/DirPaths.hh"
#include "Environment/SingleBehaviorFactory.hh"

#include "Framework/PathAppender.hh"
#include "Framework/DataProcessing.hh"
#include "Framework/SubSystemStatus.hh"
#include "Framework/MethodCommandProvider.hh"
#include "Framework/MeshData.hh"
#include "Framework/PhysicalChemicalLibrary.hh"

#include "FiniteVolume/CellCenterFVM.hh"

#include "FiniteVolumeRadiation/FiniteVolumeRadiation.hh"
#include "FiniteVolumeRadiation/Radiation.hh"


/////////////////////////////////////////////////////////////////////////////

using namespace std;
using namespace COOLFluiD::Framework;
using namespace COOLFluiD::Common;
using namespace COOLFluiD::MathTools;
using namespace COOLFluiD::Numerics::FiniteVolume;

//////////////////////////////////////////////////////////////////////////////

namespace COOLFluiD {

  namespace Numerics {

    namespace FiniteVolumeRadiation {

//////////////////////////////////////////////////////////////////////////////

MethodCommandProvider<Radiation, DataProcessingData, FiniteVolumeRadiationModule>
RadiationProvider("Radiation");

//////////////////////////////////////////////////////////////////////////////

Radiation::Radiation(const std::string& name) :
  DataProcessingCom(name),
  socket_states("states"),
  socket_gstates("gstates"),
  socket_nstates("nstates"),
  socket_volumes("volumes"),
  socket_nodes("nodes"),
  socket_isOutward("isOutward"),
  socket_normals("normals"),
  socket_CellID("CellID"),
  socket_divq("divq"),
  socket_qx("qx"),
  socket_qy("qy"),
  socket_qz("qz"),
  socket_TempProfile("TempProfile"),
  m_library(CFNULL),
  m_geoBuilder(), 
  m_faceBuilder(),
  m_normal(),
  m_weight(),
  m_fieldSource(),
  m_fieldAbsor(),
  m_fieldAbSrcV(),
  m_fieldAbV(),
  m_In(),
  m_II(),
  m_opacities(),
  m_radSource(),
  m_Ttable(),
  m_Ptable(),
  m_sdone(),
  m_cdone(),
  m_dirs(),
  m_advanceOrder(),
  m_q(),
  m_divq(),
  m_qrAv(),
  m_divqAv(),
  m_nbDirTypes()
{
  addConfigOptionsTo(this);
  
  m_nbDirs = 8;
  this->setParameter("nDirs",&m_nbDirs);
  
  m_dirName = Environment::DirPaths::getInstance().getWorkingDir();
  setParameter("DirName", &m_dirName);
  
  m_binTabName = "air-100.dat";
  setParameter("BinTabName", &m_binTabName); 
  
  m_outTabName = "air-100.out";
  setParameter("OutTabName", &m_outTabName);
  
  m_writeToFile = true;
  setParameter("WriteToFile", &m_writeToFile);
    
  m_useExponentialMethod = true;
  setParameter("UseExponentialMethod", &m_useExponentialMethod);
  
  m_radialData = false;
  setParameter("RadialData", &m_radialData);
  
  m_oldAlgo = true;
  setParameter("OldAlgorithm", &m_oldAlgo);
  
  m_Nr = 100;
  setParameter("NbRadialPoints", &m_Nr);
  
  m_constantP = -1.; // negative by default to force to give pressure if needed
  setParameter("ConstantP", &m_constantP);
  
  m_Tmin = 1000.;
  setParameter("Tmin", &m_Tmin);
  
  m_Tmax = 12000.;
  setParameter("Tmax", &m_Tmax);

  m_deltaT = 0.0071;
  setParameter("DeltaT", &m_deltaT);
  
  m_PID = 0;
  setParameter("PID", &m_PID);
  
  m_TID = 4;
  setParameter("TID", &m_TID);
  
  m_nbThreads = 1;
  setParameter("NbThreads", &m_nbThreads);
  
  m_threadID = 0;
  setParameter("ThreadID", &m_threadID);
  
  m_loopOverBins = true;
  setParameter("LoopOverBins", &m_loopOverBins);
  
  m_emptyRun = false;
  setParameter("EmptyRun", &m_emptyRun);
}
      
//////////////////////////////////////////////////////////////////////////////

Radiation::~Radiation()
{
}

//////////////////////////////////////////////////////////////////////////////

void Radiation::defineConfigOptions(Config::OptionList& options)
{
  options.addConfigOption< CFuint >
    ("nDirs","Number of directions. Only allowed 8, 24, 48 and 80");
  options.addConfigOption< boost::filesystem::path >
    ("DirName","Name of the directories where the .dat file is located.");
  options.addConfigOption< string >
    ("BinTabName","Name of the .dat file");
  options.addConfigOption< bool >
    ("WriteToFile","Writing the table in ASCII");   
  options.addConfigOption< string >
    ("OutTabName","Name of the output file");    
  options.addConfigOption< bool >
    ("UseExponentialMethod","Exponential method for radiation. Explained in ICCFD7-1003");
  options.addConfigOption< bool >
    ("RadialData","radial q and divQ for the sphere case");
  options.addConfigOption< bool >
    ("OldAlgorithm","old algorithm (very inefficient) just kept for comparison purposes");
  options.addConfigOption< CFuint >
    ("NbRadialPoints","Number of Radial points in the sphere case");
  options.addConfigOption< CFreal >("ConstantP","Constant pressure temperature");
  options.addConfigOption< CFreal >("Tmin","Minimum temperature");
  options.addConfigOption< CFreal >("Tmax","Maximum temperature");
  options.addConfigOption< CFreal >("DeltaT","Temperature interval");
  options.addConfigOption< CFuint >("PID","ID of pressure in the state vector");
  options.addConfigOption< CFuint >("TID","ID of temperature in the state vector");
  options.addConfigOption< CFuint >("NbThreads","Number of threads/CPUs in which the algorithm has to be split.");
  options.addConfigOption< CFuint >("ThreadID","ID of the current thread within the parallel algorithm."); 
  options.addConfigOption< bool >("LoopOverBins","Loop over bins and then over directions (do the opposite if =false).");
  options.addConfigOption< bool >("EmptyRun","Run without actually solving anything, just for testing purposes.");
}
      
//////////////////////////////////////////////////////////////////////////////

std::vector<Common::SafePtr<BaseDataSocketSource> >
Radiation::providesSockets()
{
  std::vector<Common::SafePtr<BaseDataSocketSource> > result;

  result.push_back(&socket_CellID);
  result.push_back(&socket_divq);
  result.push_back(&socket_qx);
  result.push_back(&socket_qy);
  result.push_back(&socket_qz);
  result.push_back(&socket_TempProfile);
  
  return result;
}

//////////////////////////////////////////////////////////////////////////////

std::vector<Common::SafePtr<BaseDataSocketSink> >
Radiation::needsSockets()
{
  std::vector<Common::SafePtr<BaseDataSocketSink> > result;
  
  result.push_back(&socket_states);
  result.push_back(&socket_gstates);
  result.push_back(&socket_nstates);
  result.push_back(&socket_volumes);
  result.push_back(&socket_nodes);
  result.push_back(&socket_isOutward);
  result.push_back(&socket_normals);  
  
  return result;
}

//////////////////////////////////////////////////////////////////////////////

void Radiation::setup()
{
  CFAUTOTRACE;
  
  CFLog(VERBOSE, "Radiation::setup() => start\n");
  CFLog(VERBOSE, "Radiation::setup() => [threadID / nbThreads] = [" 
	<< m_threadID << " / " << m_nbThreads << "]\n");
  
  cf_assert(m_PID < PhysicalModelStack::getActive()->getNbEq());
  cf_assert(m_TID < PhysicalModelStack::getActive()->getNbEq());
  cf_assert(m_PID != m_TID);
  
  const std::string nsp = getMethodData().getNamespace();
  cf_assert(PE::GetPE().GetProcessorCount(nsp) == 1);
  
  // Setting up the file containing the binary table with opacities
  CFLog(VERBOSE, "Radiation::setup() => m_dirName = "<< m_dirName <<"\n"); 
  CFLog(VERBOSE, "Radiation::setup() => m_binTabName = "<< m_binTabName <<"\n");
  
  m_inFileHandle = Environment::SingleBehaviorFactory<Environment::FileHandlerInput>::getInstance().create();
  m_binTableFile = m_dirName / boost::filesystem::path(m_binTabName);
  
  CFLog(VERBOSE, "Radiation::setup() => m_binTableFile = "<< m_binTableFile <<"\n");
  
  // Check to force an allowed number of directions
  if ((m_nbDirs != 8) && (m_nbDirs != 24) && (m_nbDirs != 48) && (m_nbDirs != 80)) {
    CFLog(WARN, "Radiation::setup() => This ndirs is not allowed. 8 directions is chosen \n");
    m_nbDirs = 8;
  } 
  
  // Reading the table
  readOpacities();
  
  const CFuint DIM = 3;
  
  // Selecting the # of direction types depending on the option
  switch(m_nbDirs) {
  case 8:     
    m_nbDirTypes = 1;
    break;
  case 24:
    m_nbDirTypes = 1;    
    break;
  case 48:
    m_nbDirTypes = 2;
    break;
  case 80:
    m_nbDirTypes = 3;
    break;
  default:		//Case nDirs == 8
    m_nbDirTypes = 1;    
    break;
  }
  
  //Resizing the vectors
  m_weight.resize(m_nbDirs);
  
  // Get number of cells
  Common::SafePtr<Common::ConnectivityTable<CFuint> > cells =
    MeshDataStack::getActive()->getConnectivity("cellStates_InnerCells");

  const CFuint nbCells = cells->nbRows();
  
  m_sdone.resize(nbCells);
  m_cdone.resize(nbCells);
  if(m_useExponentialMethod){
    m_fieldSource.resize(nbCells);
    m_fieldAbsor.resize(nbCells);
  }
  else{
    m_fieldSource.resize(nbCells);
    m_fieldAbSrcV.resize(nbCells);
    m_fieldAbV.resize(nbCells);
  }
  m_In.resize(nbCells);
  m_II.resize(nbCells);
  
  for (CFuint iCell = 0; iCell < nbCells; iCell++){
    m_sdone[iCell] = false;
    m_cdone[iCell] = false;
  }
  
  // m_nbThreads, m_threadID
  cf_assert(m_nbDirs > 0);
  cf_assert(m_nbBins > 0);
  
  // set the start/end bins for this process
  if (m_nbThreads == 1) { 
    m_startEndDir.first  = 0;
    m_startEndBin.first  = 0;
    m_startEndDir.second = m_nbDirs-1;
    m_startEndBin.second = m_nbBins-1;
  }
  else {
    const CFuint nbBinDir     = m_nbBins*m_nbDirs; 
    const CFuint minNbThreadsPerProc = nbBinDir/m_nbThreads;
    const CFuint maxNbThreadsPerProc = minNbThreadsPerProc + nbBinDir%m_nbThreads;
    cf_assert(minNbThreadsPerProc > 0);
    
    // same direction has same meshdata structure, therefore if you have 
    // m_nbThreads <= m_nbDirs it is more scalable to split by direction 
    const CFuint startThread = m_threadID*minNbThreadsPerProc;
    const CFuint nbThreadsPerProc = (m_threadID < m_nbThreads-1) ? 
      minNbThreadsPerProc : maxNbThreadsPerProc; 
    const CFuint endThread = startThread + nbThreadsPerProc;
    
    CFLog(VERBOSE, "Radiation::setup() => nbBins, nbDirs   = [" << m_nbBins << ", " << m_nbDirs << "]\n");
    CFLog(VERBOSE, "Radiation::setup() => startThread      = [" << startThread << "]\n");
    CFLog(VERBOSE, "Radiation::setup() => endThread        = [" << endThread << "]\n");
    CFLog(VERBOSE, "Radiation::setup() => nbThreadsPerProc = [" << nbThreadsPerProc << "]\n");
    
    if (m_loopOverBins) {
      // suppose you sweep all entries while walking row-wise through 
      // a matrix (b,d) of size m_nbBins*m_nbDirs, consider the corresponding 
      // (b_start,d_start) and (b_end,d_end) points
      m_startEndDir.first = startThread%m_nbDirs; 
      m_startEndBin.first = startThread/m_nbDirs;
      m_startEndDir.second = (endThread-1)%m_nbDirs;
      m_startEndBin.second = (endThread-1)/m_nbDirs;
    }
    else {
      // suppose you sweep all entries while walking row-wise through 
      // a matrix (d,b) of size m_nbDirs*m_nbBins, consider the corresponding 
      // (d_start,b_start) and (d_end,b_end) points
      m_startEndDir.first = startThread/m_nbBins; 
      m_startEndBin.first = startThread%m_nbBins;
      m_startEndDir.second = (endThread-1)/m_nbBins;
      m_startEndBin.second = (endThread-1)%m_nbBins;
    }
  }
  
  const CFuint startBin = m_startEndBin.first;
  const CFuint endBin   = m_startEndBin.second+1;
  cf_assert(endBin <= m_nbBins);
  const CFuint startDir = m_startEndDir.first;
  const CFuint endDir   = m_startEndDir.second+1;
  cf_assert(endDir <= m_nbDirs);

  if (m_loopOverBins) {
    CFLog(INFO, "Radiation::setup() => start/end Bin = [" << startBin << ", " << endBin << "]\n");
    CFLog(INFO, "Radiation::setup() => start/end Dir = [" << startDir << ", " << endDir << "]\n");
    CFLog(INFO, "Radiation::setup() => full (Bin, Dir) list: \n");
    
    for(CFuint ib = startBin; ib < endBin; ++ib) {
      const CFuint dStart = (ib != startBin) ? 0 : startDir;
      const CFuint dEnd   = (ib != m_startEndBin.second) ? m_nbDirs : endDir;
      for (CFuint d = dStart; d < dEnd; ++d) {
	CFLog(INFO, "(" << ib << ", " << d <<"), ");
      }
      CFLog(INFO, "\n");
    }
    CFLog(INFO, "\n");
  }
  else {
    CFLog(INFO, "Radiation::setup() => start/end Dir = [" << startDir << ", " << endDir << "]\n");
    CFLog(INFO, "Radiation::setup() => start/end Bin = [" << startBin << ", " << endBin << "]\n");
    CFLog(INFO, "Radiation::setup() => full (Dir, Bin) list: \n");
    
    for(CFuint d = startDir; d < endDir; ++d) {
      const CFuint bStart = (d != startDir) ? 0 : startBin;
      const CFuint bEnd   = (d != m_startEndDir.second) ? m_nbBins : endBin;
      for (CFuint ib = bStart; ib < bEnd; ++ib) {
	CFLog(INFO, "(" << d << ", " << ib <<"), ");
      }
      CFLog(INFO, "\n");
    }
    CFLog(INFO, "\n");
  }
  
  m_dirs.resize(m_nbDirs, 3);
  m_advanceOrder.resize(m_nbDirs);
  m_q.resize(nbCells, DIM);
  m_divq.resize(nbCells);
  
  cf_assert(endDir <= m_nbDirs);
  // resize only the rows corresponding to considered directions 
  for (CFuint i = startDir; i< endDir; i++) {
    m_advanceOrder[i].resize(nbCells);
  }
  
  m_geoBuilder.setup();
  m_geoBuilder.getGeoBuilder()->setDataSockets(socket_states, socket_gstates, socket_nodes);
  CellTrsGeoBuilder::GeoData& geoData = m_geoBuilder.getDataGE();
  geoData.trs = MeshDataStack::getActive()->getTrs("InnerCells");
  
  m_normal.resize(DIM, 0.); 
  
  DataHandle<CFreal> CellID = socket_CellID.getDataHandle();
  DataHandle<CFreal> divQ = socket_divq.getDataHandle();
  DataHandle<CFreal> qx = socket_qx.getDataHandle();
  DataHandle<CFreal> qy = socket_qy.getDataHandle();
  DataHandle<CFreal> qz = socket_qz.getDataHandle();
  DataHandle<CFreal> TempProfile = socket_TempProfile.getDataHandle();
  divQ.resize(nbCells);
  CellID.resize(nbCells);
  TempProfile.resize(nbCells);
  qx.resize(nbCells);
  qy.resize(nbCells);
  qz.resize(nbCells);
  divQ   = 0.0;
  qx     = 0.0;
  qy     = 0.0;
  qz     = 0.0;
  CellID = 0.0;
  TempProfile = 0.0;
  
  //Averages for the Sphere case
  m_qrAv.resize(m_Nr);
  m_divqAv.resize(m_Nr);
  m_qrAv   = 0;
  m_divqAv = 0;  
  
  Stopwatch<WallTime> stp;
  
  stp.start();
  getDirections();
  CFLog(INFO, "Radiation::setup() => getDirections() took " << stp.read() << "s\n");
  
  stp.start();
  
  if (!m_emptyRun) {
    // only get advance order for the considered directions
    for (CFuint d = startDir; d < endDir; d++){
      cf_assert(m_advanceOrder[d].size() == nbCells);
      getAdvanceOrder(d, m_advanceOrder[d]);
    }
  }
  
  // old
  // getAdvanceOrder();
  
  CFLog(INFO, "Radiation::setup() => getAdvanceOrder() took " << stp.read() << "s\n");
  
  CFLog(VERBOSE, "Radiation::setup() => end\n");
}

//////////////////////////////////////////////////////////////////////////////

void Radiation::getDirections()
{
  CFLog(DEBUG_MIN, "Radiation::getDirections() => start\n");
  
  const CFreal pi = MathTools::MathConsts::CFrealPi();
  
  CFLog(VERBOSE, "Radiation::getDirections() => Number of Directions = " << m_nbDirs << "\n");
  
  const CFreal overSq3 = 1./std::sqrt(3.);
  switch(m_nbDirs) {
  case 8:
    m_weight[0] = 4.*pi/m_nbDirs;
    m_dirs(0,0) = overSq3;
    m_dirs(0,1) = overSq3;
    m_dirs(0,2) = overSq3;
    break;
  case 24:
    m_weight[0] = 4.*pi/m_nbDirs;
    m_dirs(0,0) = 0.2958759;
    m_dirs(0,1) = 0.2958759;
    m_dirs(0,2) = 0.9082483;
    break;
  case 48:
    m_weight[0] = 0.1609517;
    m_dirs(0,0) = 0.1838670;
    m_dirs(0,1) = 0.1838670;
    m_dirs(0,2) = 0.9656013;
    m_weight[1] = 0.3626469;
    m_dirs(1,0) = 0.1838670;
    m_dirs(1,1) = 0.6950514;
    m_dirs(1,2) = 0.6950514;
    break;
  case 80:
    m_weight[0] = 0.1712359;
    m_dirs(0,0) = 0.1422555;
    m_dirs(0,1) = 0.1422555;
    m_dirs(0,2) = 0.9795543;
    m_weight[1] = 0.0992284;
    m_dirs(1,0) = 0.1422555;
    m_dirs(1,1) = overSq3;
    m_dirs(1,2) = 0.8040087;
    m_weight[2] = 0.4617179;
    m_dirs(2,0) = overSq3;
    m_dirs(2,1) = overSq3;
    m_dirs(2,2) = overSq3;
    break;
  default:	// nDirs = 8
    m_weight[0] = 4.*pi/m_nbDirs;
    m_dirs(0,0) = overSq3;
    m_dirs(0,1) = overSq3;
    m_dirs(0,2) = overSq3;
    break;
  }
  
  CFuint d = m_nbDirTypes - 1; //Note that it has been changed, because the counters start at 0
  for (CFuint dirType = 0; dirType < m_nbDirTypes; dirType++){
    for (CFuint p = 0; p <= 2; p++){
      const CFuint l = p;	    //Note that it's different because the counter starts at 0
      const CFuint m = (p+1) % 3; //Note a % b is the remainder of the division a/b
      const CFuint n = (p+2) % 3;
      
      if (p == 0 || m_dirs(dirType,0) != m_dirs(dirType,1) ||
	  m_dirs(dirType,1) != m_dirs(dirType,2) || m_dirs(dirType,2) != m_dirs(dirType,0)) {
        CFLog(VERBOSE, "Case1::dirTypes = " << dirType <<"\n");
	CFLog(DEBUG_MIN, "l = " << l << "m = " << m << "n = " << n  <<"\n");
	for (int i = 0; i <= 1; i++) {
	  for (int j = 0; j <= 1; j++) {
	    for (int k = 0; k <= 1; k++) {
	      if ( p+i+j+k != 0) {
		//Note that this is different because the counters are different
		d += 1;
		m_weight[d] = m_weight[dirType];
		m_dirs(d,0) = std::pow(-1.,i)*m_dirs(dirType,l);
		m_dirs(d,1) = std::pow(-1.,j)*m_dirs(dirType,m);
		m_dirs(d,2) = std::pow(-1.,k)*m_dirs(dirType,n);
		CFLog(DEBUG_MIN, "l = " << l << " m = " << m << " n = " << n  <<"\n");
		CFLog(DEBUG_MIN, "d = " << d <<"\n");
		CFLog(DEBUG_MIN, "dirs[" << d <<"] = ("<<  m_dirs(d,0) <<", " << m_dirs(d,1) <<", "<<m_dirs(d,2)<<")\n");
	      }
	    }
	  }
	}
      }     
      if (m_dirs(dirType,0) != m_dirs(dirType,1) && m_dirs(dirType,1) != m_dirs(dirType,2) 
	  && m_dirs(dirType,2) != m_dirs(dirType,0)) {
	CFLog(VERBOSE, "Case2::dirTypes = " << dirType <<"\n");
	CFLog(DEBUG_MIN, "l = " << l << "m = " << m << "n = " << n  <<"\n");
	for (int i = 0; i <= 1; i++) {
	  for (int j = 0; j <= 1; j++) {
	    for (int k = 0; k <= 1; k++) {
	      //Note that this is different because the counters are different
	      d += 1;
	      m_weight[d] = m_weight[dirType];
	      m_dirs(d,0) = std::pow(-1.,i)*m_dirs(dirType,l);
	      m_dirs(d,1) = std::pow(-1.,j)*m_dirs(dirType,m);
	      m_dirs(d,2) = std::pow(-1.,k)*m_dirs(dirType,n);
	      CFLog(DEBUG_MIN, "l = " << l << " m = " << m << " n = " << n  <<"\n");
	      CFLog(DEBUG_MIN, "d = " << d <<"\n");
	      CFLog(DEBUG_MIN, "dirs[" << d <<"] = ("<<  m_dirs(d,0) <<", " << m_dirs(d,1) <<", "<<m_dirs(d,2)<<")\n");
	    }
	  }
	}
      }          
    }
  }
  
  // Printing the Directions for debugging
  for (CFuint dir = 0; dir < m_nbDirTypes; dir++) {
    CFLog(DEBUG_MIN, "Direction[" << dir <<"] = (" << m_dirs(dir,0) <<", " << m_dirs(dir,1) <<", " << m_dirs(dir,2) <<")\n");
  }
  
  CFLog(DEBUG_MIN, "Radiation::getDirections() => end\n");
}

//////////////////////////////////////////////////////////////////////////////
      
void Radiation::execute()
{
  CFAUTOTRACE;
  
  CFLog(VERBOSE, "Radiation::execute() => start\n");
  
  if (!m_emptyRun) {
    // only one CPU allow for namespace => the mesh has not been partitioned
    cf_assert(PE::GetPE().GetProcessorCount(getMethodData().getNamespace()) == 1);
    
    // Compute the order of advance
    // Call the function to get the directions
    m_q    = 0.0;
    m_divq = 0.0;
    m_II   = 0.0;
    
    const CFuint startBin = m_startEndBin.first;
    const CFuint endBin   = m_startEndBin.second+1;
    cf_assert(endBin <= m_nbBins);
    const CFuint startDir = m_startEndDir.first;
    const CFuint endDir   = m_startEndDir.second+1;
    cf_assert(endDir <= m_nbDirs);
    
    if (m_loopOverBins) {
      for(CFuint ib = startBin; ib < endBin; ++ib) {
	CFLog(VERBOSE, "Radiation::execute() => bin [" << ib << "]\n");
	// old algorithm: opacities computed for all cells at once for a given bin
	if (m_oldAlgo) {getFieldOpacities(ib);}
	const CFuint dStart = (ib != startBin) ? 0 : startDir;
	const CFuint dEnd = (ib != m_startEndBin.second)? m_nbDirs : endDir;
	for (CFuint d = dStart; d < dEnd; ++d) {
	  computeQ(ib,d);
	}
      }
    }
    else {
      for (CFuint d = startDir; d < endDir; ++d) {
	CFLog(VERBOSE, "Radiation::execute() => dir [" << d << "]\n");
	const CFuint bStart = (d != startDir) ? 0 : startBin;
	const CFuint bEnd   = (d != m_startEndDir.second) ? m_nbBins : endBin;
	for(CFuint ib = startBin; ib < endBin; ++ib) {
	  CFLog(VERBOSE, "Radiation::execute() => bin [" << ib << "]\n");
	  // old algorithm: opacities computed for all cells at once for a given bin
	  if (m_oldAlgo) {getFieldOpacities(ib);}
	  computeQ(ib,d);
	}
      }
    }
    
    DataHandle<CFreal> volumes = socket_volumes.getDataHandle();
    DataHandle<CFreal> divQ = socket_divq.getDataHandle();
    DataHandle<CFreal> qx   = socket_qx.getDataHandle();
    DataHandle<CFreal> qy   = socket_qy.getDataHandle();
    DataHandle<CFreal> qz   = socket_qz.getDataHandle();
    
    SafePtr<TopologicalRegionSet> cells = m_geoBuilder.getDataGE().trs;
    const CFuint nbCells = cells->getLocalNbGeoEnts();
    for (CFuint iCell = 0; iCell < nbCells; iCell++) {
      m_divq[iCell] = m_divq[iCell]/(volumes[iCell]); //converting area from m^3 into cm^3
      divQ[iCell] = m_divq[iCell];
      qx[iCell] = m_q(iCell,XX);
      qy[iCell] = m_q(iCell,YY);
      qz[iCell] = m_q(iCell,ZZ);
    }
    
    if (m_radialData){
      writeRadialData();
    } 
  }
  
  CFLog(VERBOSE, "Radiation::execute() => end\n");
}
      
//////////////////////////////////////////////////////////////////////////////

void Radiation::getFieldOpacities(CFuint ib)
{
  CFLog(VERBOSE, "Radiation::getFieldOpacities() => start\n");
  
  if(m_useExponentialMethod){
    m_fieldSource = 0.;
    m_fieldAbsor  = 0.;
  }
  else{
    m_fieldSource = 0.;
    m_fieldAbSrcV = 0.;
    m_fieldAbV    = 0.;
  }
  
  DataHandle<CFreal> TempProfile    = socket_TempProfile.getDataHandle();
  DataHandle<CFreal> volumes        = socket_volumes.getDataHandle();
  DataHandle<State*, GLOBAL> states = socket_states.getDataHandle();
  
  const CFuint nbCells = states.size();
  const CFuint totalNbEqs = PhysicalModelStack::getActive()->getNbEq();
  
  for (CFuint iCell = 0; iCell < nbCells; iCell++) {
    const State *currState = states[iCell];
    //Get the field pressure and T commented because now we impose a temperature profile
    CFreal p = 0.;
    CFreal T = 0.;
    
    if (m_constantP < 0.) {
      cf_assert(m_PID < currState->size());
      cf_assert(m_TID < currState->size());
      p = (*currState)[m_PID];
      T = (*currState)[m_TID];
    }
    else {
      cf_assert(m_constantP > 0.);
      cf_assert(m_Tmax > 0.);
      cf_assert(m_Tmin > 0.);
      cf_assert(m_deltaT > 0.);
      
      const CFreal r  = currState->getCoordinates().norm2();
      p = m_constantP; //Pressure in Pa
      const CFreal Tmax   = m_Tmax;
      const CFreal Tmin   = m_Tmin;
      const CFreal deltaT = m_deltaT;
      const CFreal A      = std::pow(r*0.01/deltaT, 2);
      const CFreal rmax   = 1.5;
      const CFreal Amax   = std::pow(rmax*0.01/deltaT, 2);
      T = Tmax - (Tmax - Tmin)*(1 - std::exp(-A))/(1 - std::exp(-Amax));
    }
    
    TempProfile[iCell] = T;
    
    const CFreal patm   = p/101325; //converting from Pa to atm
    CFreal val1 = 0;
    CFreal val2 = 0;
    
    tableInterpolate(T, patm, ib, val1, val2); 
    
    if(m_useExponentialMethod){
      if (val1 <= 1e-30 || val2 <= 1e-30 ){
	m_fieldSource[iCell] = 1e-30;
	m_fieldAbsor[iCell]  = 1e-30;
      }
      else {
	m_fieldSource[iCell] = val2/val1;
	m_fieldAbsor[iCell]  = val1;
      }
    }
    else{
      if (val1 <= 1e-30 || val2 <= 1e-30 ){
	m_fieldSource[iCell] = 1e-30;
	m_fieldAbV[iCell]    = 1e-30*volumes[iCell]; // Volumen converted from m^3 into cm^3
      }
      else {
	m_fieldSource[iCell] = val2/val1;
	m_fieldAbV[iCell]    = val1*volumes[iCell];
      }      
      m_fieldAbSrcV[iCell]   = m_fieldSource[iCell]*m_fieldAbV[iCell];
    }
  }
  
  CFLog(VERBOSE, "Radiation::getFieldOpacities() => end\n");
}

//////////////////////////////////////////////////////////////////////////////

void Radiation::getAdvanceOrder()
{ 
  CFLog(VERBOSE, "Radiation::getAdvanceOrder() => start\n");
  
  // The order of advance calculation begins here
  DataHandle<CFreal> CellID = socket_CellID.getDataHandle();
  CellTrsGeoBuilder::GeoData& geoData = m_geoBuilder.getDataGE();
  Common::SafePtr<TopologicalRegionSet> cells = geoData.trs;
    
  const CFreal pi = MathTools::MathConsts::CFrealPi();
  const CFuint nbCells = cells->getLocalNbGeoEnts();
  const CFuint DIM = PhysicalModelStack::getActive()->getDim();
  cf_assert(DIM == DIM_3D);
  
  // Only for debugging purposes one can change the direction here
  //   m_dirs(0,0) = 1;
  //   m_dirs(0,1) = 0;
  //   m_dirs(0,2) = 0;
  
  CFLog(DEBUG_MIN, "Radiation::getAdvanceOrder() => Computing order of advance. Before the loop over the directions\n");
  
 directions_loop:
  for (CFuint d = 0; d < m_nbDirs; d++){
    CFLog(INFO, "Radiation::getAdvanceOrder() => Direction number [" << d <<"]\n");
    CFuint mLast = 0;
    CFuint m = 0;
    CFuint stage = 1;
    // Initializing the sdone and cdone for each direction
    m_sdone.assign(nbCells, false);
    m_cdone.assign(nbCells, false);
    
    while (m < nbCells) { //The loop over the cells begins
      mLast = m;	  //Checking to see if it counts all the cells
      for (CFuint iCell = 0; iCell < nbCells; iCell++) {
	CFLog(DEBUG_MAX, "Radiation::getAdvanceOrder() => iCell = " << iCell <<"\n");
	if (m_sdone[iCell] == false) {
	  geoData.idx = iCell;
	  GeometricEntity* currCell = m_geoBuilder.buildGE();
	  const CFuint elemID = currCell->getState(0)->getLocalID();	
	  const vector<GeometricEntity*>& faces = *currCell->getNeighborGeos();
	  const CFuint nbFaces = faces.size();
	  
	  for (CFuint iFace = 0; iFace < nbFaces; ++iFace) {
	    const GeometricEntity *const face = currCell->getNeighborGeo(iFace);
	    setFaceNormal(face->getID(), elemID);
	    const CFreal dotMult = m_normal[XX]*m_dirs(d,XX) + m_normal[YY]*m_dirs(d,YY) + m_normal[ZZ]*m_dirs(d,ZZ);
	    
	    State *const neighborState = (currCell->getState(0) == face->getState(0)) ? face->getState(1) : face->getState(0);
	    const CFuint neighborID = neighborState->getLocalID();
	    const bool neighborIsSdone =  m_sdone[neighborID] || neighborState->isGhost(); // AL: this could lead to a very subtle BUG
	    // first check if it is a ghost, otherwise it could fail if m_sdone for the neighborID of the ghost was already set to "true" and this is not a ghost
	    // ghosts have local IDs but they restart from 0 up to the maximum number of ghosts 
	    // const bool neighborIsSdone =  neighborState->isGhost() || m_sdone[neighborID]; 
	  
	    // cout << "is ghost = " << neighborState->isGhost() << endl;
	  
	    //If the dot product is negative and the neighbor is not done, it skips the cell and continue the loop
	    if (dotMult < 0. && neighborIsSdone == false) {goto cell_loop;}
	  }// end loop over the FACES
	  
	  // cout << "m_advanceOrder[" << d << "][" << m <<"] = " << iCell << endl;
	  
	  m_advanceOrder[d][m] = iCell;
	  CellID[iCell] = stage;
	  m += 1;
	  m_cdone[elemID] = true;
	}// end if(Cell is not done)
	
      cell_loop:
	m_geoBuilder.releaseGE();
      }// end of the loop over the CELLS
      
      const string msg = "m_advanceOrder[" + StringOps::to_str(d) + "] = ";
      CFLog(DEBUG_MAX, CFPrintContainer<vector<int> >(msg, &m_advanceOrder[d]) << "\n");
      
      if (m == mLast) {		//Check that it wrote a cell in the current stage
	  std::cout << "No cell added to advance list in direction number = " << d <<". Problem with mesh.\n";
	  //cf_assert(m != mLast);
	  /// Test to rotate the directions
	  CFreal xAngleRotation;
	  CFreal yAngleRotation;
	  CFreal zAngleRotation;
	  std::cout << "Try rotating in x. Introduce an angle in degrees\n";
	  std::cout << "Theta = \n";
	  std::cin >> xAngleRotation;
	  std::cout << "Try rotating in y. Introduce an angle in degrees\n";
	  std::cout << "Phi = \n";
	  std::cin >> yAngleRotation;
	  std::cout << "Try rotating in z. Introduce an angle in degrees\n";
	  std::cout << "Psi = \n";
	  std::cin >> zAngleRotation;
	  
	  xAngleRotation *= pi/180;
	  yAngleRotation *= pi/180;
	  zAngleRotation *= pi/180;
	  
	  for(CFuint dirs = 0; dirs < m_nbDirs; dirs++){
	    //Rotating over x
	    const CFreal rot0 = m_dirs(dirs,0);
	    const CFreal rot1 = m_dirs(dirs,1)*std::cos(xAngleRotation) - m_dirs(dirs,2)*std::sin(xAngleRotation);
	    const CFreal rot2 = m_dirs(dirs,1)*std::sin(xAngleRotation) + m_dirs(dirs,2)*std::cos(xAngleRotation);
	    //Rotating over y
	    const CFreal rot3 = rot0*std::cos(yAngleRotation) + rot2*std::sin(yAngleRotation);
	    const CFreal rot4 = rot1;
	    const CFreal rot5 = -rot0*std::sin(yAngleRotation) + rot2*std::cos(yAngleRotation);
	    //Rotating over z
	    const CFreal rot6 = rot3*std::cos(zAngleRotation) - rot4*std::sin(zAngleRotation);
	    const CFreal rot7 = rot3*std::sin(zAngleRotation) + rot4*std::cos(zAngleRotation);
	    const CFreal rot8 = rot5;
	    
	    m_dirs(dirs,0) = rot6;
	    m_dirs(dirs,1) = rot7;
	    m_dirs(dirs,2) = rot8;
	    CFLog(VERBOSE, "dirs[" << dirs <<"] = ("<<  m_dirs(dirs,0) <<", " << m_dirs(dirs,1) <<", "<<m_dirs(dirs,2)<<")\n");
	  }
	  goto directions_loop;
	  CFLog(ERROR, "Radiation::getAdvanceOrder() => No cell added to advance list. Problem with mesh\n");
	  cf_assert(m != mLast);
      }
      m_advanceOrder[d][m - 1] = - m_advanceOrder[d][m - 1];
      m_sdone = m_cdone;
      
      CFLog(VERBOSE, "Radiation::getAdvanceOrder() => m  "<< m << " \n");
      CFLog(VERBOSE, "Radiation::getAdvanceOrder() => End of the "<< stage << " stage\n");
      
      ++stage;
    }// end of the loop over the STAGES
    //Printing advanceOrder for debug porpuses
    const string msg = "Radiation::getAdvanceOrder() => m_advanceOrder[" + StringOps::to_str(d) + "] = ";
    CFLog(DEBUG_MIN, CFPrintContainer<vector<int> >(msg, &m_advanceOrder[d]) << "\n");
  } //end for for directions
  
  CFLog(VERBOSE, "Radiation::getAdvanceOrder() => end\n");
}
      
//////////////////////////////////////////////////////////////////////////////

void Radiation::getAdvanceOrder(const CFuint d, vector<int>& advanceOrder)
{
  CFLog(VERBOSE, "Radiation::getAdvanceOrder() => start\n");
  
  // The order of advance calculation begins here
  DataHandle<CFreal> CellID = socket_CellID.getDataHandle();
  CellTrsGeoBuilder::GeoData& geoData = m_geoBuilder.getDataGE();
  Common::SafePtr<TopologicalRegionSet> cells = geoData.trs;
  
  const CFreal pi = MathTools::MathConsts::CFrealPi();
  const CFuint nbCells = cells->getLocalNbGeoEnts();
  const CFuint DIM = PhysicalModelStack::getActive()->getDim();
  cf_assert(DIM == DIM_3D);
  
  CFLog(INFO, "Radiation::getAdvanceOrder() => Direction number [" << d <<"]\n");
  
  CFuint mLast = 0;
  CFuint m = 0;
  CFuint stage = 1;
  // Initializing the sdone and cdone for each direction
  m_sdone.assign(nbCells, false);
  m_cdone.assign(nbCells, false);
  
  while (m < nbCells) { //The loop over the cells begins
    mLast = m;	  //Checking to see if it counts all the cells
    for (CFuint iCell = 0; iCell < nbCells; iCell++) {
      CFLog(DEBUG_MAX, "Radiation::getAdvanceOrder() => iCell = " << iCell <<"\n");
      if (m_sdone[iCell] == false) {
	geoData.idx = iCell;
	GeometricEntity* currCell = m_geoBuilder.buildGE();
	const CFuint elemID = currCell->getState(0)->getLocalID();	
	const vector<GeometricEntity*>& faces = *currCell->getNeighborGeos();
	const CFuint nbFaces = faces.size();
	
	for (CFuint iFace = 0; iFace < nbFaces; ++iFace) {
	  const GeometricEntity *const face = currCell->getNeighborGeo(iFace);
	  setFaceNormal(face->getID(), elemID);
	  const CFreal dotMult = getDirDotNA(d);
	  State *const neighborState = (currCell->getState(0) == face->getState(0)) ? face->getState(1) : face->getState(0);
	  const CFuint neighborID = neighborState->getLocalID();
	  const bool neighborIsSdone =  m_sdone[neighborID] || neighborState->isGhost(); // AL: this could lead to a very subtle BUG
	  // first check if it is a ghost, otherwise it could fail if m_sdone for the neighborID of the ghost was already set to "true" and this is not a ghost
	  // ghosts have local IDs but they restart from 0 up to the maximum number of ghosts 
	  // const bool neighborIsSdone =  neighborState->isGhost() || m_sdone[neighborID]; 
	  // cout << "is ghost = " << neighborState->isGhost() << endl;
	  
	  //If the dot product is negative and the neighbor is not done, it skips the cell and continue the loop
	  if (dotMult < 0. && neighborIsSdone == false) {goto cell_loop;}
	}// end loop over the FACES
	
	// cout << "advanceOrder[" << d << "][" << m <<"] = " << iCell << endl;
	
	advanceOrder[m] = iCell;
	CellID[iCell] = stage;
	m += 1;
	m_cdone[elemID] = true;
      }// end if(Cell is not done)
      
    cell_loop:
      m_geoBuilder.releaseGE();
    }// end of the loop over the CELLS
    
    const string msg = "advanceOrder[" + StringOps::to_str(d) + "] = ";
    CFLog(DEBUG_MAX, CFPrintContainer<vector<int> >(msg, &advanceOrder) << "\n");
    
    if (m == mLast) {		//Check that it wrote a cell in the current stage
      std::cout << "No cell added to advance list in direction number = " << d <<". Problem with mesh.\n";
      //cf_assert(m != mLast);
      /// Test to rotate the directions
      CFreal xAngleRotation;
      CFreal yAngleRotation;
      CFreal zAngleRotation;
      std::cout << "Try rotating in x. Introduce an angle in degrees\n";
      std::cout << "Theta = \n";
      std::cin >> xAngleRotation;
      std::cout << "Try rotating in y. Introduce an angle in degrees\n";
      std::cout << "Phi = \n";
      std::cin >> yAngleRotation;
      std::cout << "Try rotating in z. Introduce an angle in degrees\n";
      std::cout << "Psi = \n";
      std::cin >> zAngleRotation;
      
      xAngleRotation *= pi/180;
      yAngleRotation *= pi/180;
      zAngleRotation *= pi/180;
      
      for(CFuint dirs = 0; dirs < m_nbDirs; dirs++){
	//Rotating over x
	const CFreal rot0 = m_dirs(dirs,0);
	const CFreal rot1 = m_dirs(dirs,1)*std::cos(xAngleRotation) - m_dirs(dirs,2)*std::sin(xAngleRotation);
	const CFreal rot2 = m_dirs(dirs,1)*std::sin(xAngleRotation) + m_dirs(dirs,2)*std::cos(xAngleRotation);
	//Rotating over y
	const CFreal rot3 = rot0*std::cos(yAngleRotation) + rot2*std::sin(yAngleRotation);
	const CFreal rot4 = rot1;
	const CFreal rot5 = -rot0*std::sin(yAngleRotation) + rot2*std::cos(yAngleRotation);
	//Rotating over z
	const CFreal rot6 = rot3*std::cos(zAngleRotation) - rot4*std::sin(zAngleRotation);
	const CFreal rot7 = rot3*std::sin(zAngleRotation) + rot4*std::cos(zAngleRotation);
	const CFreal rot8 = rot5;
	
	m_dirs(dirs,0) = rot6;
	m_dirs(dirs,1) = rot7;
	m_dirs(dirs,2) = rot8;
	CFLog(VERBOSE, "dirs[" << dirs <<"] = ("<<  m_dirs(dirs,0) <<", " << m_dirs(dirs,1) <<", "<<m_dirs(dirs,2)<<")\n");
      }
      CFLog(ERROR, "Radiation::getAdvanceOrder() => No cell added to advance list. Problem with mesh\n");
      return; // goto directions_loop;
      cf_assert(m != mLast);
    }
    advanceOrder[m - 1] = - advanceOrder[m - 1];
    m_sdone = m_cdone;
    
    CFLog(VERBOSE, "Radiation::getAdvanceOrder() => m  "<< m << " \n");
    CFLog(VERBOSE, "Radiation::getAdvanceOrder() => End of the "<< stage << " stage\n");
    
    ++stage;
  }// end of the loop over the STAGES
  
  //Printing advanceOrder for debug purpuses
  const string msg = "Radiation::getAdvanceOrder() => advanceOrder[" + StringOps::to_str(d) + "] = ";
  CFLog(DEBUG_MIN, CFPrintContainer<vector<int> >(msg, &advanceOrder) << "\n");
  
  CFLog(VERBOSE, "Radiation::getAdvanceOrder() => end\n");
}
      
//////////////////////////////////////////////////////////////////////////////

void Radiation::readOpacities()
{
  CFLog(VERBOSE, "Radiation::readOpacities() => start\n");
  
  fstream& is = m_inFileHandle->openBinary(m_binTableFile);
  
  // the three first numbers are #bins, #Temps, #pressures
  vector<double> data(3);
  is.read((char*)&data[0], 3*sizeof(double));
  
  m_nbBins  = ((int) data[0]);
  m_nbTemp  = ((int) data[1]);
  m_nbPress = ((int) data[2]);
  
  vector<double> Pressures(m_nbPress);
  is.read((char*)&Pressures[0], m_nbPress*sizeof(double));
  
  vector<double> Temperatures(m_nbTemp);
  is.read((char*)&Temperatures[0], m_nbTemp*sizeof(double));
  
  // in the table there are 143 zeros 
  vector<double> Zeros(143);
  is.read((char*)&Zeros[0], 143*sizeof(double));
  
  // Reading the table: for each pressure
  // each bin: value1(Temp1) value2(Temp1) ... value1(TempN) value2(TempN)
  vector< vector<double> > bins(m_nbPress*m_nbBins,vector<double>(2*m_nbTemp)); //initialize  
  for (CFuint ib =0; ib < m_nbPress*m_nbBins; ++ib){
    is.read((char*)&bins[ib][0], 2*m_nbTemp*sizeof(double));
  }  
  
  double end;
  is.read((char*)&end, sizeof(double));
  
  // Verifying that the last number is zero, so the table is finished
  cf_assert(int(end) == 0);
  
  is.close();
  //}
  CFLog(VERBOSE, "Radiation::readOpacities() => After closing the binary File\n");
  
  // Storing the opacities and the radiative source into memory
  m_opacities.resize(m_nbPress*m_nbBins*m_nbTemp);
  m_radSource.resize(m_nbPress*m_nbBins*m_nbTemp);
  
  // Setting up the temperature and pressure tables
  m_Ttable.resize(m_nbTemp);
  m_Ptable.resize(m_nbPress);
  
  for(CFuint ib = 0; ib < m_nbBins; ++ib) {
    for(CFuint ip = 0; ip < m_nbPress; ++ip) {
      for(CFuint it = 0; it < m_nbTemp; ++it) {
	m_opacities[it + ib*m_nbTemp + ip*m_nbBins*m_nbTemp] = bins[ip + ib*m_nbPress][2*it];
	m_radSource[it + ib*m_nbTemp + ip*m_nbBins*m_nbTemp] = bins[ip + ib*m_nbPress][2*it + 1];
      }
    }
  }
  for(CFuint it = 0; it < m_nbTemp; ++it){
    m_Ttable[it] = Temperatures[it];
  }
  
  for(CFuint ip = 0; ip < m_nbPress; ++ip){
    m_Ptable[ip] = Pressures[ip];
  }
  
  // Writing the table into a file
  if(m_writeToFile){
    CFLog(VERBOSE, "Radiation::readOpacities() => Writing file \n");
    boost::filesystem::path file = m_dirName / boost::filesystem::path(m_outTabName);
    file = PathAppender::getInstance().appendParallel( file );
    
    SelfRegistPtr<Environment::FileHandlerOutput> fhandle = Environment::SingleBehaviorFactory<Environment::FileHandlerOutput>::getInstance().create();
    ofstream& fout = fhandle->open(file);
    
    fout <<"#Bins = "<< data[0] <<"\t#Temps = "<< data[1] <<"\t#Pressures = "<< data[2] << endl;
    
    fout << endl;
    fout <<"Pressures[atm]  = ";
    for(CFuint ip = 0; ip < m_nbPress; ++ip){
      fout << Pressures[ip] << " ";
    }
    fout << endl;
    fout << endl;
    fout <<"Temperatures[K] = ";
    for(CFuint it = 0; it < m_nbTemp; ++it){
      fout << Temperatures[it] << " ";
    }
    fout << endl;
    CFuint m = 0;
    fout.precision(18);
    for(CFuint ip = 0; ip < m_nbPress; ++ip){
      fout << endl;
      fout <<"Pressure = "<< Pressures[ip] << endl;
      fout <<"bin \t\t\t\t Temp \t\t\t\t val1 \t\t\t\t\t\t val2" << endl;
      CFuint beginLoop = m*ip;
      for(CFuint ib = 0; ib < m_nbBins; ++ib){
	for(CFuint it = 0; it < m_nbTemp; ++it){
	  fout << ib + 1 <<"\t\t\t\t" << Temperatures[it] <<"\t\t\t\t" 
	       << m_opacities[it + ib*m_nbTemp + ip*m_nbBins*m_nbTemp] 
	       <<"\t\t\t\t" << m_radSource[it + ib*m_nbTemp + ip*m_nbBins*m_nbTemp] << endl;
	}
      }
      m += m_nbBins; 
    }
    fhandle->close();     
  }
  
  CFLog(VERBOSE, "Radiation::readOpacities() => end\n");
}

//////////////////////////////////////////////////////////////////////////////
      
void Radiation::tableInterpolate(CFreal T, CFreal p, CFuint ib, CFreal& val1, CFreal& val2)
{
  
  //Find the lower bound fo the temperature and the pressure ranges
  //we assume that the temperature and pressure always fall in the bounds.
  //If they don't then the value are still interpolated from the nearest
  //two points in the temperature or pressure list
  CFuint it = m_nbTemp - 2;
  for (CFuint i = 1; i < (m_nbTemp - 2); i++){
    if(m_Ttable[i] > T) { it = i - 1; break;}
  }
  
  CFuint ip = m_nbPress - 2;
  for (CFuint i = 1; i < (m_nbPress - 2); i++){
    if(m_Ptable[i] > p) { ip = i - 1; break;}
  }
  
  //Linear interpolation for the pressure
  
  const CFuint iPiBiT           = it + ib*m_nbTemp + ip*m_nbBins*m_nbTemp;
  const CFuint iPplus1iBiT      = it + ib*m_nbTemp + (ip + 1)*m_nbBins*m_nbTemp;
  const CFuint iPiBiTplus1      = (it + 1) + ib*m_nbTemp + ip*m_nbBins*m_nbTemp;
  const CFuint iPplus1iBiTplus1 = (it + 1) + ib*m_nbTemp + (ip + 1)*m_nbBins*m_nbTemp;
  
  // Linear interpolation for the pressure
  // Interpolation of the opacities
  const CFreal bt1op = (m_opacities[iPplus1iBiT] - m_opacities[iPiBiT])*
		    (p - m_Ptable[ip])/(m_Ptable[ip + 1] - m_Ptable[ip]) + m_opacities[iPiBiT];
  
  const CFreal bt2op = (m_opacities[iPplus1iBiTplus1] - m_opacities[iPiBiTplus1])*
		    (p - m_Ptable[ip])/(m_Ptable[ip + 1] - m_Ptable[ip]) + m_opacities[iPiBiTplus1];
  
  // Interpolation of the source
  const CFreal bt1so = (m_radSource[iPplus1iBiT] - m_radSource[iPiBiT])*
		    (p - m_Ptable[ip])/(m_Ptable[ip + 1] - m_Ptable[ip]) + m_radSource[iPiBiT];
  
  const CFreal bt2so = (m_radSource[iPplus1iBiTplus1] - m_radSource[iPiBiTplus1])*
		    (p - m_Ptable[ip])/(m_Ptable[ip + 1] - m_Ptable[ip]) + m_radSource[iPiBiTplus1];    
  
  // Logarithmic interpolation for the temperature
  // Protect against log(0) and x/0 by switching to linear interpolation if either
  // bt1 or bt2 == 0.  (Note we can't allow log of negative numbers either)
  // Interpolation of the opacities   
  if(bt1op <= 0 || bt2op <= 0){
    val1 = (bt2op - bt1op)*(T - m_Ttable[it])/(m_Ttable[it + 1] - m_Ttable[it]) + bt1op;
//    cout <<"\nOption1 \n";
//    cout <<"T = "<< T <<"\tTi+1 = "<<m_Ttable[it + 1]<<"\tTi = "<<m_Ttable[it] <<"\n";
//    cout <<"val1 = " << val1 <<"\tbt2op ="<< bt2op <<"\tbt1op ="<< bt1op <<"\n";
  }
  else {
    val1 = std::exp((T - m_Ttable[it])/(m_Ttable[it + 1] - m_Ttable[it])*std::log(bt2op/bt1op))*bt1op;
//     cout <<"\nOption2 \n";
//     cout <<"T = "<< T <<"\tTi+1 = "<<m_Ttable[it + 1]<<"\tTi = "<<m_Ttable[it] <<"\n";
//     cout <<"val1 = " << val1 <<"\tbt2op ="<< bt2op <<"\tbt1op ="<< bt1op <<"\n";
  }
  // Interpolation of the source
  if(bt1so <= 0 || bt2so <= 0){
    val2 = (bt2so - bt1so)*(T - m_Ttable[it])/(m_Ttable[it + 1] - m_Ttable[it]) + bt1so;
//     cout <<"\nOption3 \n";
//     cout <<"T = "<< T <<"\tTi+1 = "<<m_Ttable[it + 1]<<"\tTi = "<<m_Ttable[it] <<"\n";
//     cout <<"val1 = " << val2 <<"\tbt2so ="<< bt2so <<"\tbt1so ="<< bt1so <<"\n";
  }
  else {
    val2 = std::exp((T - m_Ttable[it])/(m_Ttable[it + 1] - m_Ttable[it])*std::log(bt2so/bt1so))*bt1so;
//     cout <<"\nOption3 \n";
//     cout <<"T = "<< T <<"\tTi+1 = "<<m_Ttable[it + 1]<<"\tTi = "<<m_Ttable[it] <<"\n";
//     cout <<"val2 = " << val2 <<"\tbt2so ="<< bt2so <<"\tbt1so ="<< bt1so <<"\n";
  }
  
  //cf_assert(ib == 0);
  
}
//////////////////////////////////////////////////////////////////////////////

void Radiation::writeRadialData()
{
  CFLog(VERBOSE, "Radiation::writeRadialData() = > Writing radial data for the spherical test case => start\n");
  
  boost::filesystem::path file = m_dirName / boost::filesystem::path("radialData.plt");
  file = PathAppender::getInstance().appendParallel( file );
  
  SelfRegistPtr<Environment::FileHandlerOutput> fhandle = 
    Environment::SingleBehaviorFactory<Environment::FileHandlerOutput>::getInstance().create();
  ofstream& outputFile = fhandle->open(file);
  
  outputFile << "TITLE  = Radiation radial data for a sphere\n";
  outputFile << "VARIABLES = r  qr divq nbPoints\n";

  CellTrsGeoBuilder::GeoData& geoData = m_geoBuilder.getDataGE();
  Common::SafePtr<TopologicalRegionSet> cells = geoData.trs;
  
  const CFuint nbCells = cells->getLocalNbGeoEnts();
  CFuint nbPoints = 0;
  CFreal rCoord = 0.;
  const CFreal Radius = 1.5;
  
  for(CFuint ir = 0; ir < m_Nr; ir++){
    nbPoints = 0;
    rCoord = (ir + 0.5)*Radius/m_Nr; //middle point between ir and (ir + 1)
    
    for(CFuint iCell = 0; iCell < nbCells; iCell++){
      geoData.idx = iCell;
      GeometricEntity* currCell = m_geoBuilder.buildGE();
      
      Node& coordinate = currCell->getState(0)->getCoordinates();
      CFreal x = coordinate[XX];
      CFreal y = coordinate[YY];
      CFreal z = coordinate[ZZ];
      
      CFreal rCell = std::sqrt(x*x + y*y + z*z);
      
      if(rCell >= ir*Radius/m_Nr && rCell < (ir + 1)*Radius/m_Nr){
	nbPoints++;
	m_divqAv[ir] += m_divq[iCell];
	m_qrAv[ir]   += (m_q(iCell,XX)*x + m_q(iCell,YY)*y + m_q(iCell,ZZ)*z)/rCell; //*rCell*rCell; Multiply by r**2 for area-weighted average
      }
      m_geoBuilder.releaseGE();
    }
    if(nbPoints > 0){
      m_divqAv[ir] /= nbPoints;
      m_qrAv[ir]   /= nbPoints; //m_qrAv[ir]   /= nbPoints*rCoord*rCoord; //area-weighted average radial flux
      outputFile << rCoord << " " << m_qrAv[ir] << " " << m_divqAv[ir] << " " <<  nbPoints << "\n";
    }
  }
  fhandle->close();
  
  CFLog(VERBOSE, "Radiation::writeRadialData() => Writing radial data for the spherical test case => end\n");
}

//////////////////////////////////////////////////////////////////////////////

void Radiation::unsetup()
{
  CFAUTOTRACE;
}

//////////////////////////////////////////////////////////////////////////////

void Radiation::getFieldOpacities(const CFuint ib, const CFuint iCell, 
				  GeometricEntity* const currCell)
{
  m_fieldSource[iCell] = 0.;
  if(m_useExponentialMethod){
    m_fieldAbsor[iCell] = 0.;
  }
  else{
    m_fieldAbSrcV[iCell] = 0.;
    m_fieldAbV[iCell]    = 0.;
  }
  
  DataHandle<CFreal> TempProfile = socket_TempProfile.getDataHandle();
  DataHandle<CFreal> volumes     = socket_volumes.getDataHandle();
  
  const State *currState = currCell->getState(0); // please note the reference &
  //Get the field pressure and T commented because now we impose a temperature profile
  CFreal p = 0.;
  CFreal T = 0.;
  if (m_constantP < 0.) {
    cf_assert(m_PID < currState->size());
    cf_assert(m_TID < currState->size());
    p = (*currState)[m_PID];
    T = (*currState)[m_TID];
  }
  else {
    cf_assert(m_constantP > 0.);
    cf_assert(m_Tmax > 0.);
    cf_assert(m_Tmin > 0.);
    cf_assert(m_deltaT > 0.);
    
    const CFreal r  = currState->getCoordinates().norm2();
    p = m_constantP; //Pressure in Pa
    const CFreal Tmax   = m_Tmax;
    const CFreal Tmin   = m_Tmin;
    const CFreal deltaT = m_deltaT;
    const CFreal A      = std::pow(r*0.01/deltaT, 2);
    const CFreal rmax   = 1.5;
    const CFreal Amax   = std::pow(rmax*0.01/deltaT, 2);
    T = Tmax - (Tmax - Tmin)*(1 - std::exp(-A))/(1 - std::exp(-Amax));
  }
  
  TempProfile[iCell] = T;
  
  const CFreal patm   = p/101325.; //converting from Pa to atm
  CFreal val1 = 0;
  CFreal val2 = 0;
  
  tableInterpolate(T, patm, ib, val1, val2); 
  
  if(m_useExponentialMethod){
    if (val1 <= 1e-30 || val2 <= 1e-30 ){
      m_fieldSource[iCell] = 1e-30;
      m_fieldAbsor[iCell]  = 1e-30;
    }
    else {
      m_fieldSource[iCell] = val2/val1;
      m_fieldAbsor[iCell]  = val1;
    }
  }
  else{
    if (val1 <= 1e-30 || val2 <= 1e-30 ){
      m_fieldSource[iCell] = 1e-30;
      m_fieldAbV[iCell]    = 1e-30*volumes[iCell]; // Volume converted from m^3 into cm^3
    }
    else {
      m_fieldSource[iCell] = val2/val1;
      m_fieldAbV[iCell]    = val1*volumes[iCell];
    }      
    m_fieldAbSrcV[iCell]   = m_fieldSource[iCell]*m_fieldAbV[iCell];
  }
}
      
//////////////////////////////////////////////////////////////////////////////

void Radiation::computeQ(const CFuint ib, const CFuint d)
{      
  CFLog(VERBOSE, "Radiation::computeQ() in (bin, dir) = ("
	<< ib << ", " << d << ") => start\n");
  
  DataHandle<CFreal> volumes = socket_volumes.getDataHandle();
  CellTrsGeoBuilder::GeoData& geoData = m_geoBuilder.getDataGE();
  SafePtr<TopologicalRegionSet> cells = geoData.trs;
  const CFuint nbCells = cells->getLocalNbGeoEnts();
  cf_assert(m_advanceOrder[d].size() == nbCells);
  
  for (CFuint m = 0; m < nbCells; m++) {
    CFreal inDirDotnANeg = 0.;
    CFreal Ic            = 0.;
    
    // allocate the cell entity
    const CFuint iCell = std::abs(m_advanceOrder[d][m]);
    geoData.idx = iCell;    
    GeometricEntity *const currCell = m_geoBuilder.buildGE();
    
    // new algorithm (more parallelizable): opacities are computed cell by cell
    // for a given bin
    if (!m_oldAlgo) {getFieldOpacities(ib, iCell, currCell);} 
    
    const CFuint elemID = currCell->getState(0)->getLocalID();	
    const vector<GeometricEntity*>& faces = *currCell->getNeighborGeos();
    const CFuint nbFaces = faces.size();
    
    if(m_useExponentialMethod){
      inDirDotnANeg = 0.;
      CFreal dirDotnANeg = 0;
      CFreal Lc      = 0;
      CFreal halfExp = 0;
      
      for (CFuint iFace = 0; iFace < nbFaces; ++iFace) {
	const GeometricEntity *const face = currCell->getNeighborGeo(iFace);
	setFaceNormal(face->getID(), elemID);
	const CFreal dirDotNA = getDirDotNA(d);
	
	if(dirDotNA < 0.) {
	  dirDotnANeg += dirDotNA;
	  State *const neighborState = (currCell->getState(0) == face->getState(0)) ? 
	    face->getState(1) : face->getState(0);
	  if(neighborState->isGhost() == false){
	    inDirDotnANeg += m_In[neighborState->getLocalID()]*dirDotNA;
	  }
	  else {
	    const CFreal boundarySource = m_fieldSource[iCell];
	    inDirDotnANeg += boundarySource*dirDotNA;
	  }
	}
      } 
      Lc          = volumes[iCell]/(- dirDotnANeg); 
      halfExp     = std::exp(-0.5*Lc*m_fieldAbsor[iCell]);
      m_In[iCell] = (inDirDotnANeg/dirDotnANeg)*halfExp*halfExp + (1. - halfExp*halfExp)*m_fieldSource[iCell];
      Ic          = (inDirDotnANeg/dirDotnANeg)*halfExp + (1. - halfExp)*m_fieldSource[iCell];
    }
    else{
      CFreal dirDotnAPos = 0;
      for (CFuint iFace = 0; iFace < nbFaces; ++iFace) {
	const GeometricEntity *const face = currCell->getNeighborGeo(iFace);
	setFaceNormal(face->getID(), elemID);
	const CFreal dirDotNA = getDirDotNA(d);
	
	if (dirDotNA >= 0.){
	  dirDotnAPos += dirDotNA;
	}
	else {
	  State *const neighborState = (currCell->getState(0) == face->getState(0)) ? face->getState(1) : face->getState(0);
	  if (neighborState->isGhost() == false){
	    inDirDotnANeg += m_In[neighborState->getLocalID()]*dirDotNA;
	  }
	  else {
	    const CFreal boundarySource = m_fieldSource[iCell];
	    inDirDotnANeg += boundarySource*dirDotNA;
	  }
	}
      } 
      m_In[iCell] = (m_fieldAbSrcV[iCell] - inDirDotnANeg)/(m_fieldAbV[iCell] + dirDotnAPos);
      Ic = m_In[iCell];
    }
    
    m_q(iCell,XX) += Ic*m_dirs(d,0)*m_weight[d];
    m_q(iCell,YY) += Ic*m_dirs(d,1)*m_weight[d];
    m_q(iCell,ZZ) += Ic*m_dirs(d,2)*m_weight[d];
    
    CFreal inDirDotnA = inDirDotnANeg;
    for (CFuint iFace = 0; iFace < nbFaces; ++iFace) {
      const GeometricEntity *const face = currCell->getNeighborGeo(iFace);
      setFaceNormal(face->getID(), elemID);
      const CFreal dirDotNA = getDirDotNA(d); 
      
      if (dirDotNA > 0.){
	inDirDotnA += m_In[iCell]*dirDotNA;
      }
    }
    
    m_divq[iCell] += inDirDotnA*m_weight[d];
    m_II[iCell]   += Ic*m_weight[d];
    
    // deallocate the cell entity
    m_geoBuilder.releaseGE();
  }  
  
  CFLog(VERBOSE, "Radiation::computeQ() in (bin, dir) = ("
	<< ib << ", " << d << ") => end\n");
}
      
//////////////////////////////////////////////////////////////////////////////
  
    } // namespace FiniteVolumeRadiation

  } // namespace Numerics

} // namespace COOLFluiD

//////////////////////////////////////////////////////////////////////////////

