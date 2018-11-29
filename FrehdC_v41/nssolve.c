#include<stdio.h>
#include<stdlib.h>
#include<time.h>
#include<math.h>
#include<string.h>

// -----------------------------------------------------------------------------
// This file contains the functions to solve the 2D depth integrated Navier
// Stokes equations (including the free surface equations).
// - ZhiLi 2017-05-05 -
// -----------------------------------------------------------------------------

#include "configuration.h"
#include "bathymetry.h"
#include "map.h"
#include "fileio.h"
#include "mpifunctions.h"
#include "nsfunctions.h"
#include "scalar.h"
#include "subgrid.h"
#include "utilities.h"

#include "laspack/errhandl.h"
#include "laspack/vector.h"
#include "laspack/matrix.h"
#include "laspack/qmatrix.h"
#include "laspack/operats.h"
#include "laspack/factor.h"
#include "laspack/precond.h"
#include "laspack/eigenval.h"
#include "laspack/rtc.h"
#include "laspack/itersolv.h"
#include "laspack/mlsolv.h"
#include "laspack/version.h"
#include "laspack/copyrght.h"

void SolveAll(Data **data, Sub **sub, Maps *map, BC *bc, Bath *bath, Config *setting, int irank, int nrank);
void oneCompleteStep(Data **data, Sub **sub, Maps *map, BC *bc, Bath *bath, Config *setting, int tt, \
  QMatrix A, Vector x, Vector z, int itank, int nrank);
void solveSourceTerm(Data **data, Sub *sub, Maps *map, BC *bc, Bath *bath, Config *setting, int tt, \
  int irank, int nrank);
void solveFreeSurface(Data **data, Sub *sub, Maps *map, BC *bc, Bath *bath, Config *setting, int tt, \
  QMatrix A, Vector x, Vector z, int irank, int nrank);
void updateData(Data **data, Sub **sub, Maps *map, BC *bc, Bath *bath, Config *setting, int tt, \
  int irank, int nrank);

// ========== The top level code of solve ==========
void SolveAll(Data **data, Sub **sub, Maps *map, BC *bc, Bath *bath, Config *setting, int irank, int nrank)
{
  int tt = 0, writeInd, root = 0;
  float t0, t1, tstep;
  // output initial condition
  writeInd = tt;
  DataOutput(data, bath, setting, writeInd, root, irank);
  if (setting->useSubgrid != 0) {SubOutput(sub, setting, writeInd, root, irank);}
  // begin time stepping
  if (irank == 0)
  {printf("Ready for time stepping ...\n");}
  for (tt = 1; tt <= setting->Nt; tt++)
  {

      //if (tt > 1200)
      //{setting->OutItvl = 1;}
    //printf("tide is %lf\n",bc->tideP[tt]);
    //bc->inflow[tt] = bc->inflow[tt] * 2.0;
    bc->inflow[tt] = 0.0;
    bc->tideP[tt] = (bc->tideP[tt] - 0.5)*2.0 + 0.5;
    //bc->tideP[tt] = 1.0 - bc->tideP[tt];
    //bc->tideP[tt] = 0.3;
    writeInd = tt;
    QMatrix A;
    Q_Constr(&A, "A", setting->N2ci, False, Rowws, Normal, True);
    Vector z;
    V_Constr(&z, "z", setting->N2ci, Normal, True);
    Vector x;
    V_Constr(&x, "x", setting->N2ci, Normal, True);
    if (irank == 0)
    {t0 = clock();}
    
    // Solve the Navier Stokes equations for the current time step
    oneCompleteStep(data, sub, map, bc, bath, setting, tt, A, x, z, irank, nrank);
    Q_Destr(&A);
    V_Destr(&x);
    V_Destr(&z);
    
    // Monitor for stability. Give warning if the maximum CFL number is
    // greater than 2 (which meant possible wetting of 3 cells in 1 step)
    monitorCFL(data, bath, irank, setting, tt, root);
    // Volume loss due to CFLlimiter, hmin, negative surface and waterfall
    // output data
    if (tt % setting->OutItvl == 0)
    {
        DataOutput(data, bath, setting, writeInd, root, irank);
        if (setting->useSubgrid != 0) {SubOutput(sub, setting, writeInd, root, irank);}
    }
    
    // report time
    if (irank == 0)
    {
      t1 = clock();
      tstep = (t1 - t0)/(float)CLOCKS_PER_SEC;
      printf("---------- Step %d executed with time %.4f sec... ----------\n",writeInd,tstep);
    }
  }
  if (irank == 0)
  {printf("Time stepping completed...\n");}
}

// ========== Function to solve 1 time step ==========
void oneCompleteStep(Data **data, Sub **sub, Maps *map, BC *bc, Bath *bath, Config *setting, int tt, \
  QMatrix A, Vector x, Vector z, int irank, int nrank)
{
  solveSourceTerm(data, *sub, map, bc, bath, setting, tt, irank, nrank);
  solveFreeSurface(data, *sub, map, bc, bath, setting, tt, A, x, z, irank, nrank);
  updateData(data, sub, map, bc, bath, setting, tt, irank, nrank);
  if (setting->useScalar == 1)
  {scalarTransport(data, map, bath, bc, *sub, setting, tt, irank, nrank);}
}

// ========== Compute the explicit source term ==========
void solveSourceTerm(Data **data, Sub *sub, Maps *map, BC *bc, Bath *bath, Config *setting, int tt, int irank, int nrank)
{
  explicitVelocity(data, map, setting);
  if (setting->useSubgrid == 1)
  {
    //if (setting->phiNonlinear == 1)
     //{advectionTermSub(data, map, sub, setting);}
    //else
     //{advectionTerm(data, map, setting);}
    advectionTerm(data, map, setting);
    diffusionTermSub(data, map, sub, setting);
    //windTermSub(data, bc, sub, setting, tt);
    windTerm(data, bc, setting, tt);
    dragTermSub(data, map, sub, setting);
  }
  else
  {
    advectionTerm(data, map, setting);
    diffusionTerm(data, map, setting);
    windTerm(data, bc, setting, tt);
    dragTerm(data, map, setting);
  }
  
  dragInversion(data, map, setting);
  if (setting->useMPI == 1)
  {
    mpiexchange((*data)->EnXP, map, setting, irank, nrank);
    mpiexchange((*data)->EnYP, map, setting, irank, nrank);
  }
  if (setting->useSubgrid != 0)
  {matrixSourceTermSub(data, map, bc, sub, setting, tt, irank);}
  else
  {matrixSourceTerm(data, map, bc, setting, tt, irank);}
}

// ========== Compute the free surface elevation ==========
void solveFreeSurface(Data **data, Sub *sub, Maps *map, BC *bc, Bath *bath, Config *setting, int tt, \
  QMatrix A, Vector x, Vector z, int irank, int nrank)
{
  if (setting->useSubgrid == 0)
  {matrixCoeff(data, map, setting);}
  else
  {matrixCoeffSub(data, map, sub, setting);}
  adjustMatrixBoundary(data, bc, map, setting, irank, nrank);
  adjustTidalBoundary(data, bc, bath, setting, tt, irank, nrank);
  setupMatrix(*data, map, setting, A);
  solveMatrix(*data, setting, A, x, z);
  getFreeSurface(data, map, setting, x);
  zeroNegSurf(data, bath, setting, tt);
  enforceFreeSurfaceBC(data, map, setting);
  enforceTidalBC(data, bath, bc, setting, tt, irank, nrank);
}

// =============== Update depth and velocity ===============
void updateData(Data **data, Sub **sub, Maps *map, BC *bc, Bath *bath, Config *setting, int tt, \
  int irank, int nrank)
{
  // ----- remove high velocity or small depth cells -----
  limiterCFL(data, bath, map, setting);
  if (setting->useMPI == 1)
  {
    mpiexchange((*data)->surf, map, setting, irank, nrank);
  }
  // ----- compute grid cell volume and scalar mass -----
  if (setting->useSubgrid != 0)
  {
    computeVolumeSub(data, *sub, setting);
    // here is where the subgrid variables are updated
    computeSubArea(sub, *data, bath, map, setting);
    if (setting->useMPI == 1)
    {
      mpiexchangeInt((*sub)->ind, map, setting, irank, nrank);
      mpiexchange((*sub)->Op, map, setting, irank, nrank);
      mpiexchange((*sub)->Om, map, setting, irank, nrank);
      mpiexchange((*sub)->Vyp, map, setting, irank, nrank);
      mpiexchange((*sub)->Vym, map, setting, irank, nrank);
    }
    combineSubArea(sub, *data, bath, map, setting, irank, nrank);
    if (setting->useMPI == 1)
    {
      mpiexchange((*sub)->Nx, map, setting, irank, nrank);
      mpiexchange((*sub)->Oy, map, setting, irank, nrank);

      mpiexchange((*sub)->V, map, setting, irank, nrank);
      mpiexchange((*sub)->Z, map, setting, irank, nrank);
      mpiexchange((*sub)->Vx, map, setting, irank, nrank);
      mpiexchange((*sub)->Vy, map, setting, irank, nrank);
    }
    updateDepth(data, map, bath, setting);
    //updateAllFaceDepthSub(data, *sub, map, setting, irank, nrank);
    updateAllFaceDepth(data, bath, map, setting, irank, nrank);
  }
  else
  {
    computeVolume(data, setting);
    updateDepth(data, map, bath, setting);
    updateAllFaceDepth(data, bath, map, setting, irank, nrank);
  }
  if (setting->useMPI == 1)
  {
    mpiexchange((*data)->depthXP, map, setting, irank, nrank);
    mpiexchange((*data)->depthYP, map, setting, irank, nrank);
  }
  // ----- update cell face velocities -----
  if (setting->useSubgrid != 0)
  {
    detectWaterfallLocationSub(data, *sub, bath, map, setting);
    updateVelocitySub(data, map, *sub, setting);
    waterfallCorrectionSub(data, *sub, bath, map, setting);
  }
  else
  {
    detectWaterfallLocation(data, bath, map, setting);
    updateVelocity(data, map, setting);
    waterfallCorrection(data, bath, map, setting);
  }
  enforceVelocityBC(data, map, setting);
  if (setting->useMPI == 1)
  {
    mpiexchange((*data)->uuXP, map, setting, irank, nrank);
    mpiexchange((*data)->vvYP, map, setting, irank, nrank);
  }
  // ----- update cell face flow rates -----
  if (setting->useSubgrid != 0)
  {
    computeFaceFlowRateSub(data, map, *sub, setting);
    adjustTidalVelocitySub(data, map, *sub, setting, irank, nrank);
    //velocityInterpSub(data, *sub, setting);
    velocityInterp(data, setting);
    volumeByFluxSub(data, *sub, map, bc, setting, tt, irank);
  }
  else
  {
    computeFaceFlowRate(data, map, setting);
    adjustTidalVelocity(data, map, setting, irank, nrank);
    velocityInterp(data, setting);
    volumeByFlux(data, map, bc, setting, tt, irank);
  }
  
  if (setting->useMPI == 1)
  {
    mpiexchange((*data)->Fuu, map, setting, irank, nrank);
    mpiexchange((*data)->Fvv, map, setting, irank, nrank);
  }

  if (setting->useSubgrid != 0 || setting->CDnotN == 2)
  {updateCDSub(data, *sub, map, setting);}
  else
  {updateCD(data, setting);}
  if (useThinLayer == 1)
  {thinLayerDrag(data, setting);}
}