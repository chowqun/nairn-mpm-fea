/*********************************************************************************************
    ConductionTask.cpp
    nairn-mpm-fea
    
    Created by John Nairn on Fri Oct 15 2004
    Copyright (c) 2004 John A. Nairn, All rights reserved.
 
	Conduction calculations
   -------------------------
	Initialization:
		Set gTemperature, gMpCp, fcond on node to zero;
	Mass and Momentum Task
		Extrapolate gTemperature and gMpCp (Task1Extrapolation())
		Divide gTemperature by gMpCp (GetNodalValue()
		Impose grid T BCs (ImposeValueBCs())
		Find grad T on particles (GetGradients())
	Grid Forces Task
		Extrapolate conductivity force to fcond (AddForces())
			(include heat sources, and energy coupling)
		Add crack tip heating to fcond (AddCrackTipHeating())
		Finish grid BCs and impose flux BCs in fcond (SetTransportForceBCs())
	Update Momenta Task
		Divide fcond by gMpCp to get temperature rates (TransportRates())
	Update Particles Task
		Each particle: zero rate, extrapolate from nodes to particle, then
			update particle (IncrementTransportRate(),MoveTransportRate()).
	Update strains last (if used)
		Update gTemperature on nodes (UpdateNodalValues())
	Update strains on particles (both places)
		Extrapolate grid temperature to particle using IncrementValueExtrap()
		Find dT = extrapolated value minus previous extrapolated value and then
			store extrapolated value in pPreviousTemperature (GetDeltaValue())
		This task is done because contititutive laws work better when temperature comes
			from grid extrapolation instead of particle temperature. The current grid
			based result is stored in a different particle temperature. The particle
			temperature, however, is the one used in conduction calculations. Using the
			grid based one causes numerical diffision.
*********************************************************************************************/

#include "stdafx.h"
#include "Custom_Tasks/ConductionTask.hpp"
#include "NairnMPM_Class/NairnMPM.hpp"
#include "Elements/ElementBase.hpp"
#include "Materials/MaterialBase.hpp"
#include "Boundary_Conditions/NodalTempBC.hpp"
#include "Boundary_Conditions/MatPtHeatFluxBC.hpp"
#include "Cracks/CrackHeader.hpp"
#include "MPM_Classes/MPMBase.hpp"
#include "Nodes/NodalPoint.hpp"
#include "Cracks/CrackSegment.hpp"
#include "Global_Quantities/ThermalRamp.hpp"
#include "Materials/RigidMaterial.hpp"
#include "Exceptions/CommonException.hpp"

// flag to activate
bool ConductionTask::active=false;
bool ConductionTask::activeRamp=false;

// flags only allowed when conduction is active
bool ConductionTask::crackTipHeating=false;
bool ConductionTask::crackContactHeating=false;
bool ConductionTask::matContactHeating=false;

// these don't require conduction to be active
bool ConductionTask::adiabatic=false;
bool ConductionTask::AVHeating=true;

// possible one conduction task
ConductionTask *conduction=NULL;

#pragma mark STANDARD METHODS

// Return name of this task
const char *ConductionTask::TaskName(void) { return "conduction calculations"; }

// called once at start of MPM analysis and after preliminary calcse are eon
TransportTask *ConductionTask::Initialize(void)
{
	// print task details
	cout << "Coupled " << TaskName() << endl;
	if(crackTipHeating)
		cout << "   Crack tip heating activated" << endl;
	if(crackContactHeating)
		cout << "   Crack contact frictional heating activated" << endl;
	if(matContactHeating)
		cout << "   Material contact frictional heating activated" << endl;
	
	// allocate diffusion data on each particle
    // done before know number of nonrigid, so do on all
	for(int p=0;p<nmpms;p++)
		mpm[p]->AllocateTemperature();
	
	return nextTask;
}

// adjust time for given cell size if needed
TransportTask *ConductionTask::TransportTimeStep(int matid,double dcell,double *tmin)
{	double diffCon=theMaterials[matid]->MaximumDiffusivity();
	double tst=(dcell*dcell)/(4.*diffCon);						// factor 2 shorter than minimum
	if(tst<*tmin) *tmin=tst;
	return nextTask;
}

#pragma mark MASS AND MOMENTUM EXTRAPOLATIONS

// Task 1 Extrapolation of temperature to the grid
TransportTask *ConductionTask::Task1Extrapolation(NodalPoint *ndpt,MPMBase *mptr,double shape)
{	double Cp = theMaterials[mptr->MatID()]->GetHeatCapacity(mptr);		// nJ/(g-K) using Cv is correct
	double arg = mptr->mp*Cp*shape;										// nJ/K
	double argT = mptr->pTemperature*arg;								// nJ
	ndpt->gTemperature += argT;
	ndpt->gMpCp += arg;
	return nextTask;
}

// Task 1 reduction of ghost node to real node for temperature on the grid
TransportTask *ConductionTask::Task1Reduction(NodalPoint *real,NodalPoint *ghost)
{	real->gTemperature += ghost->gTemperature;                    // nJ
	real->gMpCp += ghost->gMpCp;                                  // nJ/K
	return nextTask;
}

// Task 1b - get grid temperature on one node
TransportTask *ConductionTask::GetNodalValue(NodalPoint *ndptr)
{   if(ndptr->NodeHasNonrigidParticles()) ndptr->gTemperature /= ndptr->gMpCp;
	return nextTask;
}

// Task 1b - impose grid-based temperature BCs
void ConductionTask::ImposeValueBCs(double stepTime)
{
	int i;
		
	// Copy no-BC temperature
    NodalTempBC *nextBC=firstTempBC;
    while(nextBC!=NULL)
	{   i=nextBC->GetNodeNum(stepTime);
		if(i!=0) nextBC->CopyNodalTemperature(nd[i]);
        nextBC=(NodalTempBC *)nextBC->GetNextObject();
    }
	
    // Zero them all
    nextBC=firstTempBC;
    while(nextBC!=NULL)
	{   i=nextBC->GetNodeNum(stepTime);
		if(i!=0) nd[i]->gTemperature = 0.;
        nextBC=(NodalTempBC *)nextBC->GetNextObject();
    }
	
    // Now add all temperatures to nodes with temperature BCs
    nextBC=firstTempBC;
    while(nextBC!=NULL)
	{   i = nextBC->GetNodeNum(stepTime);
		if(i!=0) nd[i]->gTemperature += nextBC->BCValue(stepTime);
        nextBC=(NodalTempBC *)nextBC->GetNextObject();
    }
}

// Task 1b - get gradients in Vp * cp on particles
// throws CommonException()
TransportTask *ConductionTask::GetGradients(double stepTime)
{
    CommonException *transErr = NULL;
#ifdef CONST_ARRAYS
	int ndsArray[MAX_SHAPE_NODES];
	double fn[MAX_SHAPE_NODES],xDeriv[MAX_SHAPE_NODES],yDeriv[MAX_SHAPE_NODES],zDeriv[MAX_SHAPE_NODES];
#else
	int ndsArray[maxShapeNodes];
    double fn[maxShapeNodes],xDeriv[maxShapeNodes],yDeriv[maxShapeNodes],zDeriv[maxShapeNodes];
#endif
    
	// in case 2D planar
    for(int i=0;i<maxShapeNodes;i++) zDeriv[i] = 0.;
	
	// Find gradients on the nonrigid particles
#pragma omp parallel for private(ndsArray,fn,xDeriv,yDeriv) firstprivate(zDeriv)
    for(int p=0;p<nmpmsNR;p++)
	{	try
        {   // find shape functions and derviatives
            MPMBase *mptr = mpm[p];
            const ElementBase *elref = theElements[mptr->ElemID()];
			int *nds = ndsArray;
            elref->GetShapeGradients(fn,&nds,xDeriv,yDeriv,zDeriv,mptr);
			int i,numnds = nds[0];
            
            // Find gradients from current temperatures
            mptr->AddTemperatureGradient(GRAD_GLOBAL);			// zero gradient on the particle
            for(i=1;i<=numnds;i++)
            {	Vector deriv = MakeVector(xDeriv[i],yDeriv[i],zDeriv[i]);
                mptr->AddTemperatureGradient(GRAD_GLOBAL,ScaleVector(&deriv,nd[nds[i]]->gTemperature));
            }
			
 		}
        catch(CommonException& err)
        {   if(transErr!=NULL)
            {
#pragma omp critical (error)
                transErr = new CommonException(err);
            }
        }
    }
    
    // throw any errors
    if(transErr!=NULL) throw *transErr;
	return nextTask;
}

#pragma mark GRID FORCES EXTRAPOLATIONS

// find forces for conduction calculation (N-mm/sec = mJ/sec) (non-rigid particles only)
TransportTask *ConductionTask::AddForces(NodalPoint *ndptr,MPMBase *mptr,double sh,double dshdx,
										 double dshdy,double dshdz,TransportProperties *t)
{
	// internal force based on conduction tensor
	ndptr->fcond += mptr->FCond(GRAD_GLOBAL,dshdx,dshdy,dshdz,t);
	
	// next task
	return nextTask;
}

// copy conduction forces from ghost node to real node
TransportTask *ConductionTask::CopyForces(NodalPoint *real,NodalPoint *ghost)
{	// global force
	real->fcond += ghost->fcond;
	
	// next task
	return nextTask;
}

// adjust forces at grid points with temperature BCs to have rates be correct
// to carry extrapolated temperatures (before impose BCs) to the correct
// one selected by grid based BC
TransportTask *ConductionTask::SetTransportForceBCs(double deltime)
{
    // Paste back noBC temperature
    int i;
    NodalTempBC *nextBC=firstTempBC;
    while(nextBC!=NULL)
    {   i=nextBC->GetNodeNum(mtime);
		if(i!=0)
		{	nextBC->PasteNodalTemperature(nd[i]);
			nd[i]->fcond = 0.;
			nextBC->InitQReaction();
		}
        nextBC=(NodalTempBC *)nextBC->GetNextObject();
	}
    
    // Set force to - mp Cp T(no BC)/timestep (only once per node)
    nextBC=firstTempBC;
    while(nextBC!=NULL)
	{   i=nextBC->GetNodeNum(mtime);
		if(i!=0)
		{	if(nd[i]->fcond==0.)
			{	double qflow = -nd[i]->gMpCp*nd[i]->gTemperature/deltime;
				nd[i]->fcond = qflow;
				nextBC->SuperposeQReaction(qflow);								// for global archive of boundary flow
			}
		}
        nextBC=(NodalTempBC *)nextBC->GetNextObject();
	}
    
    // Now add each superposed temperature (* mp Cp) BC at incremented time
    nextBC=firstTempBC;
    while(nextBC!=NULL)
    {	i=nextBC->GetNodeNum(mtime);
		if(i!=0)
		{	double qflow = nd[i]->gMpCp*nextBC->BCValue(mtime)/deltime;
			nd[i]->fcond += qflow;
			nextBC->SuperposeQReaction(qflow);			// for global archive of boundary flow
		}
        nextBC=(NodalTempBC *)nextBC->GetNextObject();
    }
	
	// --------- heat flux BCs -------------
	MatPtHeatFluxBC *nextFlux=firstHeatFluxPt;
    while(nextFlux!=NULL)
    	nextFlux = nextFlux->AddMPHeatFlux(mtime);
	
	// next task
	return nextTask;
}

// add flux to tranport flow force to active nodes only
// postRateCalc means fcond has been divided by its mass
void ConductionTask::AddFluxCondition(NodalPoint *ndptr,double extraFlux,bool postRateCalc)
{	if(ndptr->NodeHasNonrigidParticles())
	{	if(postRateCalc) extraFlux /= ndptr->gMpCp;
		ndptr->fcond += extraFlux;
	}
}

#pragma mark UPDATE MOMENTA TASK

// get temperature rates node
TransportTask *ConductionTask::TransportRates(NodalPoint *ndptr,double deltime)
{	if(ndptr->NodeHasNonrigidParticles())
		ndptr->fcond /= ndptr->gMpCp;
	return nextTask;
}

#pragma mark UPDATE PARTICLES TASK

// increment temperature rate on the particle
TransportTask *ConductionTask::IncrementTransportRate(const NodalPoint *ndpt,double shape,double &rate) const
{	rate += ndpt->fcond*shape;
	return nextTask;
}

// increment particle concentration (time is always timestep)
TransportTask *ConductionTask::MoveTransportValue(MPMBase *mptr,double deltime,double rate) const
{	mptr->pTemperature += deltime*rate;
	return nextTask;
}

#pragma mark UPDATE PARTICLE STRAIN TASK

// return increment transport rate
double ConductionTask::IncrementValueExtrap(NodalPoint *ndpt,double shape) const
{	return ndpt->gTemperature*shape;
}

// after extrapolated, find change this update on particle
double ConductionTask::GetDeltaValue(MPMBase *mptr,double pTempExtrap) const
{	double dTemperature = pTempExtrap-mptr->pPreviousTemperature;
	mptr->pPreviousTemperature = pTempExtrap;
	return dTemperature;
}

#pragma mark UPDATE STRAIN LAST TASK

// if needed for SZS or USAVG, update temperature on the grid (tempTime is always timestep)
TransportTask *ConductionTask::UpdateNodalValues(double tempTime)
{	// add for each node
    for(int i=1;i<=nnodes;i++)
	{   if(nd[i]->NodeHasNonrigidParticles())
			nd[i]->gTemperature += nd[i]->fcond*tempTime;
	}
	return nextTask;
}

#pragma mark CUSTOM METHODS

// If crack tip heating activated and there are cracks
// add heat of each crack as point sources for ndpt->fcond
void ConductionTask::AddCrackTipHeating(void)
{
	if(!crackTipHeating || firstCrack==NULL) return;
	CrackHeader *nextCrack=firstCrack;
	while(nextCrack!=NULL)
	{	nextCrack->CrackTipHeating();
		nextCrack=(CrackHeader *)nextCrack->GetNextObject();
	}
}

// Tell crack tip to heat itself when it propagates
void ConductionTask::StartCrackTipHeating(CrackSegment *crkTip,Vector &grow,double thickness)
{
	if(!crackTipHeating) return;
	double dist=sqrt(grow.x*grow.x+grow.y*grow.y);
	crkTip->StartCrackTipHeating(dist,thickness);
}

#pragma mark CLASS METHODS

// conduction analysis settings
void ConductionTask::ThermodynamicsOutput(void)
{   // the system
    if(ConductionTask::IsSystemIsolated())
        cout << "System: isolated" << endl;
    else
        cout << "System: nonisolated" << endl;;
    if(active)
        cout << "Particles: nonisolated";
    else
        cout << "Particles: isolated";
    if(adiabatic)
        cout << " and adiabatic" << endl;
    else
        cout << " and isothermal" << endl;
}

// is the system isolated?
// update this if new thermal BCs are added
bool ConductionTask::IsSystemIsolated(void)
{
    // if has ramp, then is it not isolated
	if(activeRamp) return FALSE;
    
    // if no conduction then is isolated
    if(!active) return TRUE;
    
    // if conduction is active, still isolated if no thermal BC
    if(firstTempBC!=NULL) return FALSE;
    
    // check if any active rigid particles set temperature
	if(RigidMaterial::someSetTemperature) return FALSE;
    
    // must be isolated
    return TRUE;
}
