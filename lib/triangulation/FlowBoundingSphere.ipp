/*************************************************************************
*  Copyright (C) 2009 by Emanuele Catalano <catalano@grenoble-inp.fr>    *
*  Copyright (C) 2009 by Bruno Chareyre <bruno.chareyre@hmg.inpg.fr>     *
*  Copyright (C) 2012 by Donia Marzougui <donia.marzougui@grenoble-inp.fr>*
*                                                                        *
*  This program is free software; it is licensed under the terms of the  *
*  GNU General Public License v2 or later. See file LICENSE for details. *
*************************************************************************/
#ifdef FLOW_ENGINE
// #include "def_types.h"
// #include "def_flow_types.h"
// #include "CGAL/constructions/constructions_on_weighted_points_cartesian_3.h"
// #include <CGAL/Width_3.h>


// #define XVIEW
#include "FlowBoundingSphere.hpp"//include after #define XVIEW
#include <iostream>
#include <fstream>
#include <new>
#include <utility>
#include "vector"
#include <assert.h>
#include <sys/stat.h>
#include <sys/types.h>
// #include "Network.hpp"
#include <omp.h>


#ifdef XVIEW
// #include "Vue3D.h" //FIXME implicit dependencies will look for this class (out of tree) even ifndef XVIEW
#endif

#define FAST
#define TESS_BASED_FORCES
#define FACET_BASED_FORCES 1

#ifdef YADE_OPENMP
//   #define GS_OPEN_MP //It should never be defined if Yade is not using openmp
#endif

// #define USE_FAST_MATH 1


using namespace std;
namespace CGT
{

typedef vector<double> VectorR;

//! Use this factor, or minLength, to reduce max permeability values (see usage below))
const double minLength = 0.01;//percentage of mean rad

//! Factors including the effect of 1/2 symmetry in hydraulic radii
const Real multSym1 = 1/pow(2,0.25);
const Real multSym2 = 1/pow(4,0.25);

#ifdef XVIEW
Vue3D Vue1;
#endif
template<class Tesselation>
FlowBoundingSphere<Tesselation>::~FlowBoundingSphere()
{
}
template <class Tesselation> 
FlowBoundingSphere<Tesselation>::FlowBoundingSphere()
{
	x_min = 1000.0, x_max = -10000.0, y_min = 1000.0, y_max = -10000.0, z_min = 1000.0, z_max = -10000.0;
	currentTes = 0;
	nOfSpheres = 0;
	SectionArea = 0, Height=0, Vtotale=0;
	vtk_infinite_vertices=0, vtk_infinite_cells=0;
	VISCOSITY = 1;
	fluidBulkModulus = 0;
	tess_based_force = true;
	for (int i=0;i<6;i++) boundsIds[i] = 0;
	minPermLength=-1;
	SLIP_ON_LATERALS = false;//no-slip/symmetry conditions on lateral boundaries
	TOLERANCE = 1e-07;
	RELAX = 1.9;
	ks=0;
	distance_correction = true;
	clampKValues = true;
	meanKStat = true; K_opt_factor=0;
	noCache=true;
	pressureChanged=false;
	computeAllCells=true;//might be turned false IF the code is reorganized (we can make a separate function to compute unitForceVectors outside Compute_Permeability) AND it really matters for CPU time
	DEBUG_OUT = true;
	RAVERAGE = false; /** use the average between the effective radius (inscribed sphere in facet) and the equivalent (circle surface = facet fluid surface) **/
	OUTPUT_BOUDARIES_RADII = false;
	RAVERAGE = false; /** if true use the average between the effective radius (inscribed sphere in facet) and the equivalent (circle surface = facet fluid surface) **/
// 	areaR2Permeability=true;
	permeability_map = false;
	computedOnce=false;
	minKdivKmean=0.0001;
	maxKdivKmean=100.;
	ompThreads=1;
	errorCode=0;
}

template <class Tesselation> 
void FlowBoundingSphere<Tesselation>::ResetNetwork() {T[0].Clear();noCache=true;}

template <class Tesselation> 
Tesselation& FlowBoundingSphere<Tesselation>::Compute_Action()
{
        return Compute_Action(0,NULL,NULL);
}
template <class Tesselation> 
Tesselation& FlowBoundingSphere<Tesselation>::Compute_Action(int argc, char *argv[ ], char *envp[ ])
{
	double factor = 1.001;
	VectorR X, Y, Z, R;
        Real_timer clock;
        clock.start();
        clock.top("start");
        Tesselation& Tes = T[0];
        RTriangulation& Tri = Tes.Triangulation();

        /** READING SPHERES POSITIONS FROM A TEXT FILE CONTAINING COORDINATES **/
        double x, y, z, r;
        ifstream loadFile(argc==1 ? "cube" : argv[1]);    // cree l'objet loadFile de la classe ifstream qui va permettre de lire importFilename
        while (!loadFile.eof()) {
                loadFile >> x >> y >> z >> r;
                X.push_back(x);
                Y.push_back(y);
                Z.push_back(z);
                R.push_back(factor*r);
                nOfSpheres++;
                Rmoy += r;
                x_min = min(x_min,x-r);
                x_max = max(x_max,x+r);
                y_min = min(y_min,y-r);
                y_max = max(y_max,y+r);
                z_min = min(z_min,z-r);
                z_max = max(z_max,z+r);
        }
        Rmoy /= nOfSpheres;
        minPermLength = Rmoy*minLength;
	if (DEBUG_OUT) cout << "Rmoy = " << Rmoy << endl;
	if (DEBUG_OUT) cout << "x_min = " << x_min << " x_max = " << x_max << " y_min = " << y_min << " y_max = " << y_max << " y_max = " << z_min << " x_min = " << z_max << endl;

        Vertex_handle Vh;
	Cell_handle neighbour_cell, cell, location;

	int V = X.size();
        if (DEBUG_OUT) cout << "V =" << V << "nOfSpheres = " << nOfSpheres << endl;
        if (DEBUG_OUT) cout << Tes.Max_id() << endl;
        clock.top("loading spheres");


	vector<Sphere> vs; RTriangulation testT;
	for (int i=0; i<V; i++) {
		vs.push_back(Sphere(Point(X[i],Y[i],Z[i]), R[i]));
        }
        clock.top("make a spheres vector");
	testT.insert(vs.begin(),vs.end());
	clock.top("test speed");

        AddBoundingPlanes();
        for (int i=0; i<V; i++) {
                int id = Tes.Max_id() +1;
                Vh = Tes.insert(X[i],Y[i],Z[i],R[i],id);    /** EMPILEMENT QUELCONQUE **/
#ifdef XVIEW
                Vue1.SetSpheresColor(0.8,0.6,0.6,1);
                Vue1.Dessine_Sphere(X[i],Y[i],Z[i], R[i], 15);
#endif
        }
        Height = y_max-y_min;
        SectionArea = (x_max-x_min) * (z_max-z_min);
	Vtotale = (x_max-x_min) * (y_max-y_min) * (z_max-z_min);
        clock.top("Triangulation");

        Tes.Compute();
        clock.top("tesselation");

        boundary(y_min_id).flowCondition=0;
        boundary(y_max_id).flowCondition=0;
        boundary(y_min_id).value=0;
        boundary(y_max_id).value=1;
	Define_fictious_cells();
        clock.top("BoundaryConditions");

        /** INITIALIZATION OF VOLUMES AND PRESSURES **/
        Finite_cells_iterator cell_end = Tri.finite_cells_end();
        for (Finite_cells_iterator cell = Tri.finite_cells_begin(); cell != cell_end; cell++) {
		cell->info().volume() = ( std::abs ( ( CGT::Tetraedre ( cell->vertex(0)->point(),cell->vertex(1)->point(),cell->vertex(2)->point(),cell->vertex(3)->point()).volume() ) ) );
                cell->info().dv() = 0;
        }

        clock.top("initializing delta_volumes");

        /** PERMEABILITY **/
        /** START PERMEABILITY CALCULATION**/
        k_factor = 1;
        Compute_Permeability();
        clock.top("Compute_Permeability");
        /** END PERMEABILITY CALCULATION**/

	if(DEBUG_OUT) cerr << "TOTAL VOID VOLUME: " << Vporale <<endl;
	if(DEBUG_OUT) cerr << "Porosity = " << V_porale_porosity / V_totale_porosity << endl;

        /** STATISTICS **/
        DisplayStatistics();
        clock.top("DisplayStatistics");
        /** START GAUSS SEIDEL */
        //  Boundary_Conditions ( Tri );
	double P_zero = abs((boundary(y_min_id).value-boundary(y_max_id).value)/2);
        Initialize_pressures( P_zero );
	clock.top("Initialize_pressures");
        GaussSeidel();
        clock.top("GaussSeidel");
        /** END GAUSS SEIDEL */
	const char* file ="Permeability";
        ks = Permeameter(boundary(y_min_id).value, boundary(y_max_id).value, SectionArea, Height, file);
        clock.top("Permeameter");

	ComputeFacetForcesWithCache();
        clock.top("Compute_Forces");

        ///*** VUE 3D ***///
  
#ifdef XVIEW
        Vue1.SetCouleurSegments(0.1,0,1);
        Dessine_Short_Tesselation(Vue1, Tes);
        Vue1.Affiche();
#endif
	if (SLIP_ON_LATERALS && DEBUG_OUT) cout << "SLIP CONDITION IS ACTIVATED" << endl;
	else if (DEBUG_OUT) cout << "NOSLIP CONDITION IS ACTIVATED" << endl;
// }
  return Tes;
}

template <class Tesselation> 
Tesselation& FlowBoundingSphere<Tesselation>::LoadPositions(int argc, char *argv[ ], char *envp[ ])
{
	double factor = 1.001;
	VectorR X, Y, Z, R;
        Tesselation& Tes = T[0];
//         RTriangulation& Tri = Tes.Triangulation();
        /** READING SPHERES POSITIONS FROM A TEXT FILE CONTAINING COORDINATES **/
        double x, y, z, r;
        ifstream loadFile(argc==1 ? "cube" : argv[1]);    // cree l'objet loadFile de la classe ifstream qui va permettre de lire importFilename
        while (!loadFile.eof()) {
                loadFile >> x >> y >> z >> r;
                X.push_back(x);
                Y.push_back(y);
                Z.push_back(z);
                R.push_back(factor*r);
                nOfSpheres++;
                Rmoy += r;
                x_min = min(x_min,x-r);
                x_max = max(x_max,x+r);
                y_min = min(y_min,y-r);
                y_max = max(y_max,y+r);
                z_min = min(z_min,z-r);
                z_max = max(z_max,z+r);
        }
        Rmoy /= nOfSpheres;
        minPermLength = Rmoy*minLength;
        Vertex_handle Vh;
	Cell_handle neighbour_cell, cell, location;

	int V = X.size();
        if (DEBUG_OUT) cout << "V =" << V << "nOfSpheres = " << nOfSpheres << endl;
        if (DEBUG_OUT) cout << Tes.Max_id() << endl;

	AddBoundingPlanes();
        for (int i=0; i<V; i++) {
                int id = Tes.Max_id() +1;
                Vh = Tes.insert(X[i],Y[i],Z[i],R[i],id);    /** EMPILEMENT QUELCONQUE **/
#ifdef XVIEW
                Vue1.SetSpheresColor(0.8,0.6,0.6,1);
                Vue1.Dessine_Sphere(X[i],Y[i],Z[i], R[i], 15);
#endif
        }
        Height = y_max-y_min;
        SectionArea = (x_max-x_min) * (z_max-z_min);
	Vtotale = (x_max-x_min) * (y_max-y_min) * (z_max-z_min);

        Tes.Compute();
 	Define_fictious_cells();

  return Tes;
}

template <class Tesselation>
void FlowBoundingSphere<Tesselation>::Average_Relative_Cell_Velocity()
{
	RTriangulation& Tri = T[noCache?(!currentTes):currentTes].Triangulation();
        Point pos_av_facet;
        int num_cells = 0;
        double facet_flow_rate = 0;
	Finite_cells_iterator cell_end = Tri.finite_cells_end();
        for ( Finite_cells_iterator cell = Tri.finite_cells_begin(); cell != cell_end; cell++ ) {
		if (cell->info().isGhost) continue;
		cell->info().av_vel() =CGAL::NULL_VECTOR;
                num_cells++;
		Real tot_flow_rate = 0;//used to acount for influxes in elements where pressure is imposed
                for ( int i=0; i<4; i++ ) if (!Tri.is_infinite(cell->neighbor(i))){
				Vecteur Surfk = cell->info()-cell->neighbor(i)->info();
				Real area = sqrt ( Surfk.squared_length() );
				Surfk = Surfk/area;
                        	Vecteur branch = cell->vertex ( facetVertices[i][0] )->point() - cell->info();
                        	pos_av_facet = (Point) cell->info() + ( branch*Surfk ) *Surfk;
				facet_flow_rate = (cell->info().k_norm())[i] * (cell->info().shiftedP() - cell->neighbor (i)->info().shiftedP());
				tot_flow_rate += facet_flow_rate;
				cell->info().av_vel() = cell->info().av_vel() + (facet_flow_rate) * ( pos_av_facet-CGAL::ORIGIN );
		}
		//This is the influx term
		if (cell->info().Pcondition) cell->info().av_vel() = cell->info().av_vel() - (tot_flow_rate)*((Point) cell->info()-CGAL::ORIGIN );
		//now divide by volume
		cell->info().av_vel() = cell->info().av_vel() /abs(cell->info().volume());
	}
}



template <class Tesselation> 
bool FlowBoundingSphere<Tesselation>::isOnSolid  (double X, double Y, double Z)
{
  RTriangulation& Tri = T[currentTes].Triangulation();
  Finite_cells_iterator cell_end = Tri.finite_cells_end();
  for (Finite_cells_iterator cell = Tri.finite_cells_begin(); cell != cell_end; cell++) {
    for (int i=0; i<4; i++){
      double radius = sqrt(cell->vertex(i)->point().weight());
      if (X < (cell->vertex(i)->point().x()+radius) && X > (cell->vertex(i)->point().x()-radius)){
	if (Y < (cell->vertex(i)->point().y()+radius) && Y > (cell->vertex(i)->point().y()-radius)){
	  if (Z < (cell->vertex(i)->point().z()+radius) && Z > (cell->vertex(i)->point().z()-radius)){
	    return true;}}}}}
      return false;
}
template <class Tesselation> 
void FlowBoundingSphere<Tesselation>::Average_Fluid_Velocity()
{
	Average_Relative_Cell_Velocity();
	RTriangulation& Tri = T[noCache?(!currentTes):currentTes].Triangulation();
	int num_vertex = 0;
	Finite_vertices_iterator vertices_end = Tri.finite_vertices_end();
	for (Finite_vertices_iterator V_it = Tri.finite_vertices_begin(); V_it !=  vertices_end; V_it++) {
	  num_vertex++;}
	
	vector<Real> Volumes;
	vector<CGT::Vecteur> VelocityVolumes;
	VelocityVolumes.resize(num_vertex);
	Volumes.resize(num_vertex);
	
	for (Finite_vertices_iterator V_it = Tri.finite_vertices_begin(); V_it !=  vertices_end; V_it++) {
	  VelocityVolumes[V_it->info().id()]=CGAL::NULL_VECTOR;
	  Volumes[V_it->info().id()]=0.f;}
	
	Finite_cells_iterator cell_end = Tri.finite_cells_end();
	for ( Finite_cells_iterator cell = Tri.finite_cells_begin(); cell != cell_end; cell++ )
	{
	  if (cell->info().fictious()==0){
	    for (int i=0;i<4;i++){
	      VelocityVolumes[cell->vertex(i)->info().id()] =  VelocityVolumes[cell->vertex(i)->info().id()] + cell->info().av_vel()*cell->info().volume();
	      Volumes[cell->vertex(i)->info().id()] = Volumes[cell->vertex(i)->info().id()] + cell->info().volume();}
	  }}	    
	
	std::ofstream fluid_vel ("Velocity", std::ios::out);
	double Rx = (x_max-x_min) /10;
        double Ry = (y_max-y_min) /12;
	double Rz = (z_max-z_min) /20;
	Cell_handle cellula;
	
	Vecteur Velocity = CGAL::NULL_VECTOR;
	int i=0;
	for(double X=x_min+Rx;X<x_max;X+=Rx){
	  for (double Y=y_min+Ry;Y<y_max;Y+=Ry){
	    Velocity = CGAL::NULL_VECTOR; i=0;
	    for (double Z=z_min+Rz;Z<z_max;Z+=Rz){
	      cellula = Tri.locate(Point(X,Y,Z));
	      for (int y=0;y<4;y++) {if (!cellula->vertex(y)->info().isFictious) {Velocity = Velocity + (VelocityVolumes[cellula->vertex(y)->info().id()]/Volumes[cellula->vertex(y)->info().id()]);i++;}}
	    }Velocity = Velocity/i;
	    fluid_vel << X << " " << Y << " " << Velocity << endl;
	  }}
}
template <class Tesselation> 
vector<Real> FlowBoundingSphere<Tesselation>::Average_Fluid_Velocity_On_Sphere(unsigned int Id_sph)
{
	Average_Relative_Cell_Velocity();
	RTriangulation& Tri = T[noCache?(!currentTes):currentTes].Triangulation();
	
	Real Volumes; CGT::Vecteur VelocityVolumes;
	vector<Real> result;
	result.resize(3);
	
	VelocityVolumes=CGAL::NULL_VECTOR;
	Volumes=0.f;
	
	Finite_cells_iterator cell_end = Tri.finite_cells_end();
	for ( Finite_cells_iterator cell = Tri.finite_cells_begin(); cell != cell_end; cell++ )
	{
	  if (cell->info().fictious()==0){
	    for (unsigned int i=0;i<4;i++){
	      if (cell->vertex(i)->info().id()==Id_sph){
		VelocityVolumes = VelocityVolumes + cell->info().av_vel()*cell->info().volume();
		Volumes = Volumes + cell->info().volume();}}}}
		
	for (int i=0;i<3;i++) result[i] += VelocityVolumes[i]/Volumes;
	return result;
}
template <class Tesselation> 
double FlowBoundingSphere<Tesselation>::getPorePressure (double X, double Y, double Z)
{
	if (noCache && T[!currentTes].Max_id()<=0) return 0;//the engine never solved anything
	RTriangulation& Tri = T[noCache?(!currentTes):currentTes].Triangulation();
	Cell_handle cell = Tri.locate(Point(X,Y,Z));
	return cell->info().p();
}

template <class Tesselation>
double FlowBoundingSphere<Tesselation>::getCell (double X, double Y, double Z)
{
	if (noCache) {cerr<<"Triangulation does not exist. Waht did you do?!"<<endl; return -1;}
	RTriangulation& Tri = T[noCache?(!currentTes):currentTes].Triangulation();
	Cell_handle cell = Tri.locate(Point(X,Y,Z));
	return cell->info().id;
}

template <class Tesselation> 
void FlowBoundingSphere<Tesselation>::measurePressureProfile(double Wall_up_y, double Wall_down_y)
{  
	if (noCache && T[!currentTes].Max_id()<=0) return;//the engine never solved anything
	RTriangulation& Tri = T[noCache?(!currentTes):currentTes].Triangulation();
        Cell_handle permeameter;
	std::ofstream capture ("Pressure_profile", std::ios::app);
        int intervals = 5;
	int captures = 6;
        double Rz = (z_max-z_min)/intervals;
	double Ry = (Wall_up_y-Wall_down_y)/captures;

	double X=(x_max+x_min)/2;
	double Y = Wall_down_y;
	double pressure = 0.f;
	int cell=0;
	for (int i=0; i<captures; i++){
        for (double Z=min(z_min,z_max); Z<=max(z_min,z_max); Z+=abs(Rz)) {
		permeameter = Tri.locate(Point(X, Y, Z));
		pressure+=permeameter->info().p();
		cell++;
        }
        Y += Ry;
        capture  << pressure/cell << endl;}
	
}
template <class Tesselation> 
double FlowBoundingSphere<Tesselation>::averageSlicePressure(double Y)
{
  RTriangulation& Tri = T[currentTes].Triangulation();
  double P_ave = 0.f;
  int n = 0;
  double Ry = (y_max-y_min)/30;
  double Rx = (x_max-x_min)/30;
  double Rz = (z_max-z_min)/30;
  for (double X=x_min; X<=x_max+Ry/10; X=X+Rx) {
	for (double Z=z_min; Z<=z_max+Ry/10; Z=Z+Rz) {
	  P_ave+=Tri.locate(Point(X, Y, Z))->info().p();
	  n++;
	}
  }
  P_ave/=n;
  return P_ave;
}
template <class Tesselation> 
double FlowBoundingSphere<Tesselation>::averagePressure()
{
  RTriangulation& Tri = T[currentTes].Triangulation();
  double P = 0.f, Ppond=0.f, Vpond=0.f;
  int n = 0;
  for (Finite_cells_iterator cell = Tri.finite_cells_begin(); cell != Tri.finite_cells_end(); cell++) {
	P+=cell->info().p();
	n++;
	Ppond+=cell->info().p()*cell->info().volume();
	Vpond+=cell->info().volume();
  }
  P/=n;
  Ppond/=Vpond;
  return Ppond;
}


template <class Tesselation> 
void FlowBoundingSphere<Tesselation>::ComputeFacetForcesWithCache(bool onlyCache)
{
	RTriangulation& Tri = T[currentTes].Triangulation();
	Vecteur nullVect(0,0,0);
	//reset forces
	if (!onlyCache) for (Finite_vertices_iterator v = Tri.finite_vertices_begin(); v != Tri.finite_vertices_end(); ++v) v->info().forces=nullVect;

	#ifdef parallel_forces
	if (noCache) {
		perVertexUnitForce.clear(); perVertexPressure.clear();
// 		vector<const Vecteur*> exf; exf.reserve(20);
// 		vector<const Real*> exp; exp.reserve(20);
		perVertexUnitForce.resize(T[currentTes].max_id+1);
		perVertexPressure.resize(T[currentTes].max_id+1);}
	#endif
	Cell_handle neighbour_cell;
	Vertex_handle mirror_vertex;
	Vecteur tempVect;
	//FIXME : Ema, be carefull with this (noCache), it needs to be turned true after retriangulation
	if (noCache) {for (VCell_iterator cell_it=T[currentTes].cellHandles.begin(); cell_it!=T[currentTes].cellHandles.end(); cell_it++){
// 	if (noCache) for (Finite_cells_iterator cell = Tri.finite_cells_begin(); cell != cell_end; cell++) {
			Cell_handle& cell = *cell_it;
			//reset cache
			for (int k=0;k<4;k++) cell->info().unitForceVectors[k]=nullVect;

			for (int j=0; j<4; j++) if (!Tri.is_infinite(cell->neighbor(j))) {
					neighbour_cell = cell->neighbor(j);
					const Vecteur& Surfk = cell->info().facetSurfaces[j];
					//FIXME : later compute that fluidSurf only once in hydraulicRadius, for now keep full surface not modified in cell->info for comparison with other forces schemes
					//The ratio void surface / facet surface
					Real area = sqrt(Surfk.squared_length()); if (area<=0) cerr <<"AREA <= 0!!"<<endl;
					Vecteur facetNormal = Surfk/area;
					const std::vector<Vecteur>& crossSections = cell->info().facetSphereCrossSections;
					Vecteur fluidSurfk = cell->info().facetSurfaces[j]*cell->info().facetFluidSurfacesRatio[j];
					/// handle fictious vertex since we can get the projected surface easily here
					if (cell->vertex(j)->info().isFictious) {
						Real projSurf=abs(Surfk[boundary(cell->vertex(j)->info().id()).coordinate]);
						tempVect=-projSurf*boundary(cell->vertex(j)->info().id()).normal;
						cell->vertex(j)->info().forces = cell->vertex(j)->info().forces+tempVect*cell->info().p();
						//define the cached value for later use with cache*p
						cell->info().unitForceVectors[j]=cell->info().unitForceVectors[j]+ tempVect;
					}
					/// Apply weighted forces f_k=sqRad_k/sumSqRad*f
					Vecteur Facet_Unit_Force = -fluidSurfk*cell->info().solidSurfaces[j][3];
					Vecteur Facet_Force = cell->info().p()*Facet_Unit_Force;
					
					
					for (int y=0; y<3;y++) {
						cell->vertex(facetVertices[j][y])->info().forces = cell->vertex(facetVertices[j][y])->info().forces + Facet_Force*cell->info().solidSurfaces[j][y];
						//add to cached value
						cell->info().unitForceVectors[facetVertices[j][y]]=cell->info().unitForceVectors[facetVertices[j][y]]+Facet_Unit_Force*cell->info().solidSurfaces[j][y];
						//uncomment to get total force / comment to get only viscous forces (Bruno)
						if (!cell->vertex(facetVertices[j][y])->info().isFictious) {
							cell->vertex(facetVertices[j][y])->info().forces = cell->vertex(facetVertices[j][y])->info().forces -facetNormal*cell->info().p()*crossSections[j][y];
							//add to cached value
							cell->info().unitForceVectors[facetVertices[j][y]]=cell->info().unitForceVectors[facetVertices[j][y]]-facetNormal*crossSections[j][y];
						}
					}
					#ifdef parallel_forces
					perVertexUnitForce[cell->vertex(j)->info().id()].push_back(&(cell->info().unitForceVectors[j]));
					perVertexPressure[cell->vertex(j)->info().id()].push_back(&(cell->info().p()));
					#endif
			}
		}
		noCache=false;//cache should always be defined after execution of this function
		if (onlyCache) return;
	} else {//use cached values
		#ifndef parallel_forces
		for (Finite_cells_iterator cell = Tri.finite_cells_begin(); cell != cell_end; cell++) {
			for (int yy=0;yy<4;yy++) cell->vertex(yy)->info().forces = cell->vertex(yy)->info().forces + cell->info().unitForceVectors[yy]*cell->info().p();}
			
		#else
		#pragma omp parallel for num_threads(ompThreads)
		for (int vn=0; vn<= T[currentTes].max_id; vn++) {
			if (T[currentTes].vertexHandles[vn]==NULL) continue;
			Vertex_handle& v = T[currentTes].vertexHandles[vn];
// 		for (Finite_vertices_iterator v = Tri.finite_vertices_begin(); v != Tri.finite_vertices_end(); ++v){
			const int& id =  v->info().id();
			Vecteur tf (0,0,0);
			int k=0;
			for (vector<const Real*>::iterator c = perVertexPressure[id].begin(); c != perVertexPressure[id].end(); c++)
				tf = tf + (*(perVertexUnitForce[id][k++]))*(**c);
			v->info().forces = tf;
		}
		#endif
	}
	if (DEBUG_OUT) {
		Vecteur TotalForce = nullVect;
		for (Finite_vertices_iterator v = Tri.finite_vertices_begin(); v != Tri.finite_vertices_end(); ++v)	{
			if (!v->info().isFictious) TotalForce = TotalForce + v->info().forces;
			else if (boundary(v->info().id()).flowCondition==1) TotalForce = TotalForce + v->info().forces;	}
		cout << "TotalForce = "<< TotalForce << endl;}
}
template <class Tesselation> 
void FlowBoundingSphere<Tesselation>::ComputeTetrahedralForces()
{
        RTriangulation& Tri = T[currentTes].Triangulation();
        Finite_cells_iterator cell_end = Tri.finite_cells_end();
        Vecteur nullVect(0,0,0);
        bool ref = Tri.finite_cells_begin()->info().isvisited;

	for (Finite_vertices_iterator v = Tri.finite_vertices_begin(); v != Tri.finite_vertices_end(); ++v) {
                v->info().forces=nullVect;
        }

        Cell_handle neighbour_cell;
        Vertex_handle mirror_vertex;
        for (Finite_cells_iterator cell = Tri.finite_cells_begin(); cell != cell_end; cell++) {
                for (int j=0; j<4; j++) if (!Tri.is_infinite(cell->neighbor(j)) && cell->neighbor(j)->info().isvisited==ref) {
                                neighbour_cell = cell->neighbor(j);
                                const Vecteur& Surfk = cell->info().facetSurfaces[j];
                                /// handle fictious vertex since we can get the projected surface easily here
                                if (cell->vertex(j)->info().isFictious) {
                                        Real projSurf=abs(Surfk[boundary(cell->vertex(j)->info().id()).coordinate]);
                                        cell->vertex(j)->info().forces = cell->vertex(j)->info().forces -projSurf*boundary(cell->vertex(j)->info().id()).normal*cell->info().p();
                                }
                                /// handle the opposite fictious vertex (remember each facet is seen only once)
                                mirror_vertex = neighbour_cell->vertex(Tri.mirror_index(cell,j));
                                Vertex_Info& info = neighbour_cell->vertex(Tri.mirror_index(cell,j))->info();
                                if (info.isFictious) {
                                        Real projSurf=abs(Surfk[boundary(info.id()).coordinate]);
                                        info.forces = info.forces - projSurf*boundary(info.id()).normal*neighbour_cell->info().p();
                                }
                                /// Apply weighted forces f_k=sqRad_k/sumSqRad*f
                                Vecteur Facet_Force = (neighbour_cell->info().p()-cell->info().p())*Surfk*cell->info().solidSurfaces[j][3];
                                for (int y=0; y<3;y++) {
                                        cell->vertex(facetVertices[j][y])->info().forces = cell->vertex(facetVertices[j][y])->info().forces + Facet_Force*cell->info().solidSurfaces[j][y];
                                }
                        }
                cell->info().isvisited=!ref;
        }
//	if (DEBUG_OUT) cout << "tetrahedral scheme" <<endl;
//	Vecteur TotalForce = nullVect;
//	for (Finite_vertices_iterator v = Tri.finite_vertices_begin(); v != Tri.finite_vertices_end(); ++v) {
//		if (!v->info().isFictious) {
//			TotalForce = TotalForce + v->info().forces;
//		} else {
//			if (boundary(v->info().id()).flowCondition==1) TotalForce = TotalForce + v->info().forces;
//			if (DEBUG_OUT) cout << "fictious_id = " << v->info().id() << " force = " << v->info().forces << endl;
//		}
//	}
// 	if (DEBUG_OUT) cout << "TotalForce = "<< TotalForce << endl;
}
template <class Tesselation> 
void FlowBoundingSphere<Tesselation>::ApplySinusoidalPressure(RTriangulation& Tri, double Amplitude, double Average_Pressure, double load_intervals)
{
	double step = 1/load_intervals;
	Vector_Cell tmp_cells;
	tmp_cells.resize(10000);
	VCell_iterator cells_it = tmp_cells.begin();
	for (double alpha=0; alpha<1.001; alpha+=step)
	{
	  VCell_iterator cells_end = Tri.incident_cells(T[currentTes].vertexHandles[y_max_id],cells_it);
	  for (VCell_iterator it = tmp_cells.begin(); it != cells_end; it++)
	  {
	    if(!Tri.is_infinite(*it)){
	      Point& p1 = (*it)->info();
	      Cell_handle& cell = *it;
	      if (p1.x()<x_min) cell->info().p() = Average_Pressure+Amplitude;
	      else if (p1.x()>x_max) cell->info().p() = Average_Pressure-Amplitude;
	      else if (p1.x()>(x_min+alpha*(x_max-x_min)) && p1.x()<(x_min+(alpha+step)*(x_max-x_min))) cell->info().p() = Average_Pressure + (Amplitude)*(cos(alpha*M_PI));
	  }
	  }
	}
}
template <class Tesselation> 
void FlowBoundingSphere<Tesselation>::Interpolate(Tesselation& Tes, Tesselation& NewTes)
{
        Cell_handle old_cell;
        RTriangulation& Tri = Tes.Triangulation();
	for (typename Vector_Cell::iterator cell_it=NewTes.cellHandles.begin(); cell_it!=NewTes.cellHandles.end(); cell_it++){
		Cell_handle& new_cell = *cell_it;
		if (new_cell->info().Pcondition || new_cell->info().isGhost) continue;
		Vecteur center ( 0,0,0 );
		if (new_cell->info().fictious()==0) for ( int k=0;k<4;k++ ) center= center + 0.25* (Tes.vertex(new_cell->vertex(k)->info().id())->point()-CGAL::ORIGIN);
		else {
			Real boundPos=0; int coord=0;
			for ( int k=0;k<4;k++ ) {
				if (!new_cell->vertex (k)->info().isFictious) center= center+0.3333333333*(Tes.vertex(new_cell->vertex(k)->info().id())->point()-CGAL::ORIGIN);
				else {
					coord=boundary (new_cell->vertex(k)->info().id()).coordinate;
					boundPos=boundary (new_cell->vertex(k)->info().id()).p[coord];
				}
			}
			center=Vecteur(coord==0?boundPos:center[0],coord==1?boundPos:center[1],coord==2?boundPos:center[2]);
		}
                old_cell = Tri.locate(Point(center[0],center[1],center[2]));
                new_cell->info().p() = old_cell->info().shiftedP();
		new_cell->info().solute() = old_cell->info().solute();
        }
//  	Tes.Clear();//Don't reset to avoid segfault when getting pressure in scripts just after interpolation
}

template <class Tesselation> 
Real FlowBoundingSphere<Tesselation>::checkSphereFacetOverlap(const Sphere& v0, const Sphere& v1, const Sphere& v2)
{
	//First, check that v0 projection fall between v1 and v2...
	Real dist=(v0-v1)*(v2-v1);
	if (dist<0) return 0;
	Real v1v2=(v2-v1).squared_length();
	if (dist>v1v2) return 0;
	//... then, check distance
	Real m=(cross_product(v0-v1,v2-v1)).squared_length()/v1v2;
	if (m<v0.weight()) {
		Real d=2*sqrt((v0.weight()-m));
		Real teta=2*acos(sqrt(m/v0.weight()));
		return 0.5*(teta*v0.weight()-d*sqrt(m));//this is S0, we use crossSection to avoid computing an "asin"
// 		return crossSection-m*d;
	} else return 0;
}

template <class Tesselation> 
void FlowBoundingSphere<Tesselation>::Compute_Permeability()
{
	if (DEBUG_OUT)  cout << "----Computing_Permeability------" << endl;
	RTriangulation& Tri = T[currentTes].Triangulation();
	Vsolid_tot = 0, Vtotalissimo = 0, Vporale = 0, Ssolid_tot = 0, V_totale_porosity=0, V_porale_porosity=0;
	Finite_cells_iterator cell_end = Tri.finite_cells_end();

	Cell_handle neighbour_cell;

	double k=0, distance = 0, radius = 0, viscosity = VISCOSITY;
	int surfneg=0;
	int NEG=0, POS=0, pass=0;

	bool ref = Tri.finite_cells_begin()->info().isvisited;
// 	Vecteur n;
	Real meanK=0, STDEV=0, meanRadius=0, meanDistance=0;
	Real infiniteK=1e10;

// 	double volume_sub_pore = 0.f;

	for (VCell_iterator cell_it=T[currentTes].cellHandles.begin(); cell_it!=T[currentTes].cellHandles.end(); cell_it++){
		Cell_handle& cell = *cell_it;
		Point& p1 = cell->info();
		for (int j=0; j<4; j++) {
			neighbour_cell = cell->neighbor(j);
			Point& p2 = neighbour_cell->info();
			if (!Tri.is_infinite(neighbour_cell) && (neighbour_cell->info().isvisited==ref || computeAllCells)) {
				//Compute and store the area of sphere-facet intersections for later use
				Vertex_handle W [3];
				for (int kk=0; kk<3; kk++) {
					W[kk] = cell->vertex(facetVertices[j][kk]);
				}
				Sphere& v0 = W[0]->point();
				Sphere& v1 = W[1]->point();
				Sphere& v2 = W[2]->point();

				cell->info().facetSphereCrossSections[j]=Vecteur(
				   W[0]->info().isFictious ? 0 : 0.5*v0.weight()*acos((v1-v0)*(v2-v0)/sqrt((v1-v0).squared_length()*(v2-v0).squared_length())),
				   W[1]->info().isFictious ? 0 : 0.5*v1.weight()*acos((v0-v1)*(v2-v1)/sqrt((v1-v0).squared_length()*(v2-v1).squared_length())),
				   W[2]->info().isFictious ? 0 : 0.5*v2.weight()*acos((v0-v2)*(v1-v2)/sqrt((v1-v2).squared_length()*(v2-v0).squared_length())));

				pass+=1;
				Vecteur l = p1 - p2;
				distance = sqrt(l.squared_length());
// 				n = l/distance;
				if (!RAVERAGE) radius = 2* Compute_HydraulicRadius(cell, j);
				else radius = (Compute_EffectiveRadius(cell, j)+Compute_EquivalentRadius(cell,j))*0.5;
				if (radius<0) NEG++;
				else POS++;
				if (radius==0) {
					cout << "INS-INS PROBLEM!!!!!!!" << endl;
				}
// 				Real h,d;
				Real fluidArea=0;
// 				int test=0;
				if (distance!=0) {
					if (minPermLength>0 && distance_correction) distance=max(minPermLength,distance);
					const Vecteur& Surfk = cell->info().facetSurfaces[j];
					Real area = sqrt(Surfk.squared_length());
					const Vecteur& crossSections = cell->info().facetSphereCrossSections[j];
// 					if (areaR2Permeability){
//  						Real m1=sqrt((cross_product((v0-v1),v2-v1)).squared_length()/(v2-v1).squared_length());
						Real S0=0;
						S0=checkSphereFacetOverlap(v0,v1,v2);
						if (S0==0) S0=checkSphereFacetOverlap(v1,v2,v0);
						if (S0==0) S0=checkSphereFacetOverlap(v2,v0,v1);
						//take absolute value, since in rare cases the surface can be negative (overlaping spheres)
						fluidArea=abs(area-crossSections[0]-crossSections[1]-crossSections[2]+S0);
						cell->info().facetFluidSurfacesRatio[j]=fluidArea/area;
						k=(fluidArea * pow(radius,2)) / (8*viscosity*distance);
// 					} else {
// 					 cout << "WARNING! if !areaR2Permeability, facetFluidSurfacesRatio will not be defined correctly. Don't use that."<<endl;
// 					 k = (M_PI * pow(radius,4)) / (8*viscosity*distance);}

					 meanDistance += distance;
					 meanRadius += radius;
					 meanK +=  k*k_factor;

				if (k<0 && DEBUG_OUT) {surfneg+=1;
				cout<<"__ k<0 __"<<k<<" "<<" fluidArea "<<fluidArea<<" area "<<area<<" "<<crossSections[0]<<" "<<crossSections[1]<<" "<<crossSections[2] <<" "<<W[0]->info().id()<<" "<<W[1]->info().id()<<" "<<W[2]->info().id()<<" "<<p1<<" "<<p2<<" test "<<endl;}
					     
				} else  {cout <<"infinite K1!"<<endl; k = infiniteK;}//Will be corrected in the next loop

				(cell->info().k_norm())[j]= k*k_factor;
// 				(neighbour_cell->info().k_norm())[Tri.mirror_index(cell, j)]= k*k_factor;
				if (!neighbour_cell->info().isGhost) (neighbour_cell->info().k_norm())[Tri.mirror_index(cell, j)]= (cell->info().k_norm())[j];

				
// 				if(permeability_map){
// 				  Cell_handle c = cell;
// 				  cell->info().s = cell->info().s + k*distance/fluidArea*this->Volume_Pore_VoronoiFraction (c,j);
// 				  volume_sub_pore += this->Volume_Pore_VoronoiFraction (c,j);}
// 				
			}
		}
		cell->info().isvisited = !ref;
// 		if(permeability_map){cell->info().s = cell->info().s/volume_sub_pore;
// 		volume_sub_pore = 0.f;}
	}
	if (DEBUG_OUT) cout<<"surfneg est "<<surfneg<<endl;
	meanK /= pass;
	meanRadius /= pass;
	meanDistance /= pass;
	Real globalK=k_factor*meanDistance*Vporale/(Ssolid_tot*8.*viscosity);//An approximate value of macroscopic permeability, for clamping local values below
	if (DEBUG_OUT) {
		cout << "PassCompK = " << pass << endl;
		cout << "meanK = " << meanK << endl;
		cout << "globalK = " << globalK << endl;
		cout << "maxKdivKmean*globalK = " << maxKdivKmean*globalK << endl;
		cout << "minKdivKmean*globalK = " << minKdivKmean*globalK << endl;
		cout << "meanTubesRadius = " << meanRadius << endl;
		cout << "meanDistance = " << meanDistance << endl;
	}
	ref = Tri.finite_cells_begin()->info().isvisited;
	pass=0;

	if (clampKValues) for (VCell_iterator cell_it=T[currentTes].cellHandles.begin(); cell_it!=T[currentTes].cellHandles.end(); cell_it++){
		Cell_handle& cell = *cell_it;
		for (int j=0; j<4; j++) {
			neighbour_cell = cell->neighbor(j);
			if (!Tri.is_infinite(neighbour_cell) && neighbour_cell->info().isvisited==ref) {
				pass++;
				(cell->info().k_norm())[j] = max(minKdivKmean*globalK ,min((cell->info().k_norm())[j], maxKdivKmean*globalK));
				(neighbour_cell->info().k_norm())[Tri.mirror_index(cell, j)]=(cell->info().k_norm())[j];
			}
		}
	}
	if (DEBUG_OUT) cout << "PassKcorrect = " << pass << endl;

	if (DEBUG_OUT) cout << "POS = " << POS << " NEG = " << NEG << " pass = " << pass << endl;

	// A loop to compute the standard deviation of the local K distribution, and use it to include/exclude K values higher then (meanK +/- K_opt_factor*STDEV)
	if (meanKStat)
	{
		std::ofstream k_opt_file("k_stdev.txt" ,std::ios::out);
		ref = Tri.finite_cells_begin()->info().isvisited;
		pass=0;
		for (Finite_cells_iterator cell = Tri.finite_cells_begin(); cell != cell_end; cell++) {
			for (int j=0; j<4; j++) {
				neighbour_cell = cell->neighbor(j);
				if (!Tri.is_infinite(neighbour_cell) && neighbour_cell->info().isvisited==ref) {
					pass++;
					STDEV += pow(((cell->info().k_norm())[j]-meanK),2);
				}
			}cell->info().isvisited = !ref;
		}
		STDEV = sqrt(STDEV/pass);
		if (DEBUG_OUT) cout << "PassSTDEV = " << pass << endl;
		cout << "STATISTIC K" << endl;
		double k_min = 0, k_max = meanK + K_opt_factor*STDEV;
		cout << "Kmoy = " << meanK << " Standard Deviation = " << STDEV << endl;
		cout << "kmin = " << k_min << " kmax = " << k_max << endl;
		ref = Tri.finite_cells_begin()->info().isvisited;
		pass=0;
		for (Finite_cells_iterator cell = Tri.finite_cells_begin(); cell != cell_end; cell++) {
			for (int j=0; j<4; j++) {
				neighbour_cell = cell->neighbor(j);
				if (!Tri.is_infinite(neighbour_cell) && neighbour_cell->info().isvisited==ref) {
					pass+=1;
					if ((cell->info().k_norm())[j]>k_max) {
						(cell->info().k_norm())[j]=k_max;
						(neighbour_cell->info().k_norm())[Tri.mirror_index(cell, j)]= (cell->info().k_norm())[j];
					}
					k_opt_file << K_opt_factor << " " << (cell->info().k_norm())[j] << endl;
				}
			}cell->info().isvisited=!ref;
		}
		if (DEBUG_OUT) cout << "PassKopt = " << pass << endl;
	}


	if (DEBUG_OUT) {
		Finite_vertices_iterator vertices_end = Tri.finite_vertices_end();
		Real Vgrains = 0;
		int grains=0;
		for (Finite_vertices_iterator V_it = Tri.finite_vertices_begin(); V_it !=  vertices_end; V_it++) {
			if (!V_it->info().isFictious && !V_it->info().isGhost) {
				grains +=1;
				Vgrains += 1.33333333 * M_PI * pow(V_it->point().weight(),1.5);}}
		cout<<grains<<"grains - " <<"Vtotale = " << Vtotale << " Vgrains = " << Vgrains << " Vporale1 = " << (Vtotale-Vgrains) << endl;
		cout << "Vtotalissimo = " << Vtotalissimo/2 << " Vsolid_tot = " << Vsolid_tot/2 << " Vporale2 = " << Vporale/2  << " Ssolid_tot = " << Ssolid_tot << endl<< endl;
		if (!RAVERAGE) cout << "------Hydraulic Radius is used for permeability computation------" << endl << endl;
		else cout << "------Average Radius is used for permeability computation------" << endl << endl;
		cout << "-----Computed_Permeability-----" << endl;}
}

template <class Tesselation> 
vector<double> FlowBoundingSphere<Tesselation>::getConstrictions()
{
	RTriangulation& Tri = T[currentTes].Triangulation();
	vector<double> constrictions;
	for (Finite_facets_iterator f_it=Tri.finite_facets_begin(); f_it != Tri.finite_facets_end();f_it++){
		//in the periodic case, we skip facets with lowest id out of the base period
		if ( ((f_it->first->info().index < f_it->first->neighbor(f_it->second)->info().index) && f_it->first->info().isGhost)
		||  ((f_it->first->info().index > f_it->first->neighbor(f_it->second)->info().index) && f_it->first->neighbor(f_it->second)->info().isGhost)
		|| f_it->first->info().index == 0 || f_it->first->neighbor(f_it->second)->info().index == 0) continue;
		constrictions.push_back(Compute_EffectiveRadius(f_it->first, f_it->second));
	}
	return constrictions;
}

template <class Tesselation>
vector<Constriction> FlowBoundingSphere<Tesselation>::getConstrictionsFull()
{
	RTriangulation& Tri = T[currentTes].Triangulation();
	vector<Constriction> constrictions;
	for (Finite_facets_iterator f_it=Tri.finite_facets_begin(); f_it != Tri.finite_facets_end();f_it++){
		//in the periodic case, we skip facets with lowest id out of the base period
 		 if ( ((f_it->first->info().index < f_it->first->neighbor(f_it->second)->info().index) && f_it->first->info().isGhost)
		||  ((f_it->first->info().index > f_it->first->neighbor(f_it->second)->info().index) && f_it->first->neighbor(f_it->second)->info().isGhost)
		|| f_it->first->info().index == 0 || f_it->first->neighbor(f_it->second)->info().index == 0) continue;
		vector<double> rn;
		const Vecteur& normal = f_it->first->info().facetSurfaces[f_it->second];
		if (!normal[0] && !normal[1] && !normal[2]) continue;
		rn.push_back(Compute_EffectiveRadius(f_it->first, f_it->second));
		rn.push_back(normal[0]);
		rn.push_back(normal[1]);
		rn.push_back(normal[2]);
		Constriction cons (pair<int,int>(f_it->first->info().index,f_it->first->neighbor(f_it->second)->info().index),rn);
		constrictions.push_back(cons);
	}
	return constrictions;
}

template <class Tesselation> 
double FlowBoundingSphere<Tesselation>::Compute_EffectiveRadius(Cell_handle cell, int j)
{
	RTriangulation& Tri = T[currentTes].Triangulation();
        if (Tri.is_infinite(cell->neighbor(j))) return 0;

	Vecteur B = cell->vertex(facetVertices[j][1])->point().point()-cell->vertex(facetVertices[j][0])->point().point();
	Vecteur x = B/sqrt(B.squared_length());
	Vecteur C = cell->vertex(facetVertices[j][2])->point().point()-cell->vertex(facetVertices[j][0])->point().point();
	Vecteur z = CGAL::cross_product(x,C);
// 	z = z/sqrt(z.squared_length());
	Vecteur y = CGAL::cross_product(x,z);
	y = y/sqrt(y.squared_length());

	double b1[2]; b1[0] = B*x; b1[1] = B*y;
	double c1[2]; c1[0] = C*x; c1[1] = C*y;

	double rA = sqrt(cell->vertex(facetVertices[j][0])->point().weight());
	double rB = sqrt(cell->vertex(facetVertices[j][1])->point().weight());
	double rC = sqrt(cell->vertex(facetVertices[j][2])->point().weight());

	double A = ((pow(rA,2))*(1-c1[0]/b1[0])+((pow(rB,2)*c1[0])/b1[0])-pow(rC,2)+pow(c1[0],2)+pow(c1[1],2)-((pow(b1[0],2)+pow(b1[1],2))*c1[0]/b1[0]))/(2*c1[1]-2*b1[1]*c1[0]/b1[0]);
	double BB = (rA-rC-((rA-rB)*c1[0]/b1[0]))/(c1[1]-b1[1]*c1[0]/b1[0]);
	double CC = (pow(rA,2)-pow(rB,2)+pow(b1[0],2)+pow(b1[1],2))/(2*b1[0]);
	double D = (rA-rB)/b1[0];
	double E = b1[1]/b1[0];
	double F = pow(CC,2)+pow(E,2)*pow(A,2)-2*CC*E*A;

	double c = -F-pow(A,2)+pow(rA,2);
	double b = 2*rA-2*(D-BB*E)*(CC-E*A)-2*A*BB;
	double a = 1-pow((D-BB*E),2)-pow(BB,2);

	if ((pow(b,2)-4*a*c)<0){cout << "NEGATIVE DETERMINANT" << endl; }
	double reff = (-b+sqrt(pow(b,2)-4*a*c))/(2*a);
	if (reff<0) return 0;//happens very rarely, with bounding spheres most probably
	//if the facet involves one ore more bounding sphere, we return R with a minus sign
	if (cell->vertex(facetVertices[j][2])->info().isFictious || cell->vertex(facetVertices[j][1])->info().isFictious || cell->vertex(facetVertices[j][2])->info().isFictious) return -reff;
	else return reff;
}

template <class Tesselation> 
double FlowBoundingSphere<Tesselation>::Compute_EquivalentRadius(Cell_handle cell, int j)
{
	Real fluidSurf = sqrt(cell->info().facetSurfaces[j].squared_length())*cell->info().facetFluidSurfacesRatio[j];
	return sqrt(fluidSurf/M_PI);
}
template <class Tesselation> 
double FlowBoundingSphere<Tesselation>::Compute_HydraulicRadius(Cell_handle cell, int j)
{
	RTriangulation& Tri = T[currentTes].Triangulation();
        if (Tri.is_infinite(cell->neighbor(j))) return 0;
	double Vpore = this->Volume_Pore_VoronoiFraction(cell, j);
	double Ssolid = this->Surface_Solid_Pore(cell, j, SLIP_ON_LATERALS, /*reuse the same facet data*/ true);

	//handle symmetry (tested ok)
	if (SLIP_ON_LATERALS && facetNFictious>0) {
		//! Include a multiplier so that permeability will be K/2 or K/4 in symmetry conditions
		Real mult= facetNFictious==1 ? multSym1 : multSym2;
		return Vpore/Ssolid*mult;}
	return Vpore/Ssolid;
}
template <class Tesselation> 
void FlowBoundingSphere<Tesselation>::Initialize_pressures( double P_zero )
{
        RTriangulation& Tri = T[currentTes].Triangulation();
        Finite_cells_iterator cell_end = Tri.finite_cells_end();

        for (Finite_cells_iterator cell = Tri.finite_cells_begin(); cell != cell_end; cell++){
		cell->info().p() = P_zero; cell->info().dv()=0;}

        for (int bound=0; bound<6;bound++) {
                int& id = *boundsIds[bound];
		boundingCells[bound].clear();
		if (id<0) continue;
                Boundary& bi = boundary(id);
                if (!bi.flowCondition) {
                        Vector_Cell tmp_cells;
                        tmp_cells.resize(10000);
                        VCell_iterator cells_it = tmp_cells.begin();
                        VCell_iterator cells_end = Tri.incident_cells(T[currentTes].vertexHandles[id],cells_it);
                        for (VCell_iterator it = tmp_cells.begin(); it != cells_end; it++){
				(*it)->info().p() = bi.value;(*it)->info().Pcondition=true;
				boundingCells[bound].push_back(*it);
			}
                }
        }
        IPCells.clear();
        for (unsigned int n=0; n<imposedP.size();n++) {
		Cell_handle cell=Tri.locate(imposedP[n].first);
		//check redundancy
		for (unsigned int kk=0;kk<IPCells.size();kk++){
			if (cell==IPCells[kk]) cerr<<"Two imposed pressures fall in the same cell."<<endl;
			else if  (cell->info().Pcondition) cerr<<"Imposed pressure fall in a boundary condition."<<endl;}
		IPCells.push_back(cell);
		cell->info().p()=imposedP[n].second;
		cell->info().Pcondition=true;}
	pressureChanged=false;

	IFCells.clear();
	for (unsigned int n=0; n<imposedF.size();n++) {
		Cell_handle cell=Tri.locate(imposedF[n].first);
		//check redundancy
		for (unsigned int kk=0;kk<IPCells.size();kk++){
			if (cell==IPCells[kk]) cerr<<"Both flux and pressure are imposed in the same cell."<<endl;
			else if  (cell->info().Pcondition) cerr<<"Imposed flux fall in a pressure boundary condition."<<endl;}
		IFCells.push_back(cell);
		cell->info().Pcondition=false;}

}

template <class Tesselation> 
bool FlowBoundingSphere<Tesselation>::reApplyBoundaryConditions()
{
	if (!pressureChanged) return false;
        for (int bound=0; bound<6;bound++) {
                int& id = *boundsIds[bound];
		if (id<0) continue;
                Boundary& bi = boundary(id);
                if (!bi.flowCondition) {
                        for (VCell_iterator it = boundingCells[bound].begin(); it != boundingCells[bound].end(); it++){
			(*it)->info().p() = bi.value; (*it)->info().Pcondition=true;
			}
                }
        }
        for (unsigned int n=0; n<imposedP.size();n++) {
		IPCells[n]->info().p()=imposedP[n].second;
		IPCells[n]->info().Pcondition=true;}
	pressureChanged=false;
	return true;
}

template <class Tesselation> 
void FlowBoundingSphere<Tesselation>::GaussSeidel(Real dt)
{
	reApplyBoundaryConditions();
	RTriangulation& Tri = T[currentTes].Triangulation();
	int j = 0;
	double m, n, dp_max, p_max, sum_p, p_moy, dp, sum_dp;
	double compFlowFactor=0;
	vector<Real> previousP;
	previousP.resize(Tri.number_of_finite_cells());
	double tolerance = TOLERANCE;
	double relax = RELAX;
	const int num_threads=1;
	bool compressible= (fluidBulkModulus>0);
#ifdef GS_OPEN_MP
	omp_set_num_threads(num_threads);
#endif

       if(DEBUG_OUT){ cout << "tolerance = " << tolerance << endl;
        cout << "relax = " << relax << endl;}
			vector<Real> t_sum_p, t_dp_max, t_sum_dp, t_p_max;
			t_sum_dp.resize(num_threads);
			t_dp_max.resize(num_threads);
			t_p_max.resize(num_threads);
			t_sum_p.resize(num_threads);
        Finite_cells_iterator cell_end = Tri.finite_cells_end();
	#ifdef GS_OPEN_MP
		vector<Finite_cells_iterator> cells_its;
		for (Finite_cells_iterator cell = Tri.finite_cells_begin(); cell != cell_end; cell++) if ( !cell->info().Pcondition ) cells_its.push_back(cell);
		int num_cells=cells_its.size();
		cout<<"cells_its.size() "<<cells_its.size();
	#endif
// 	#pragma omp parallel shared(t_sum_dp, t_dp_max, sum_p, sum_dp,cells_its, j, Tri, relax)
	{
        do {
                int cell2=0; dp_max = 0;p_max = 0;p_moy=0;sum_p=0;sum_dp=0;
		#ifdef GS_OPEN_MP
		cell2=num_cells;
		for (int ii=0;ii<num_threads;ii++) t_p_max[ii] =0;
		for (int ii=0;ii<num_threads;ii++) t_dp_max[ii] =0;
		for (int ii=0;ii<num_threads;ii++) t_sum_p[ii]=0;
                for (int ii=0;ii<num_threads;ii++) t_sum_dp[ii]=0;
		int kk=0;
		const int num_cells2 = num_cells;
		#pragma omp parallel for private(dp, m, n, kk) shared(tolerance, t_sum_dp, t_dp_max, sum_p, sum_dp,cells_its, j, Tri, relax) schedule(dynamic, 1000)
		for (kk=0; kk<num_cells2; kk++) {
			const Finite_cells_iterator& cell = cells_its[kk];
			{
		#else
		int bb=-1;
                for (Finite_cells_iterator cell = Tri.finite_cells_begin(); cell != cell_end; cell++) {
			bb++;
			if ( !cell->info().Pcondition ) {
		                cell2++;
		#endif
				if (compressible && j==0) { previousP[bb]=cell->info().p(); }
				m=0, n=0;
				for (int j2=0; j2<4; j2++) {
					if (!Tri.is_infinite(cell->neighbor(j2))) { 
						/// COMPRESSIBLE: 
						if ( compressible ) {
							compFlowFactor = fluidBulkModulus*dt*cell->info().invVoidVolume();
							m += compFlowFactor*(cell->info().k_norm())[j2] * cell->neighbor(j2)->info().p();
							if (j==0) n +=compFlowFactor*(cell->info().k_norm())[j2];
						} else {							
						/// INCOMPRESSIBLE 
							m += (cell->info().k_norm())[j2] * cell->neighbor(j2)->info().p();
							if ( isinf(m) && j<10 ) cout << "(cell->info().k_norm())[j2] = " << (cell->info().k_norm())[j2] << " cell->neighbor(j2)->info().p() = " << cell->neighbor(j2)->info().p() << endl;
							if (j==0) n += (cell->info().k_norm())[j2];
						}  
					}
				}
				dp = cell->info().p();
				if (n!=0 || j!=0) {
					if (j==0) { if (compressible) cell->info().inv_sum_k=1/(1+n); else cell->info().inv_sum_k=1/n; }
					if ( compressible ) {
					/// COMPRESSIBLE cell->info().p() = ( (previousP - compFlowFactor*cell->info().dv()) + m ) / n ;
						cell->info().p() = ( ((previousP[bb] - ((fluidBulkModulus*dt*cell->info().invVoidVolume())*(cell->info().dv()))) + m) * cell->info().inv_sum_k - cell->info().p()) * relax + cell->info().p();
					} else {
					/// INCOMPRESSIBLE cell->info().p() =   - ( cell->info().dv() - m ) / ( n ) = ( -cell.info().dv() + m ) / n ;
						cell->info().p() = (- (cell->info().dv() - m) * cell->info().inv_sum_k - cell->info().p()) * relax + cell->info().p();
					}
					#ifdef GS_OPEN_MP
// 					double r = sqrt(sqrt(sqrt(cell->info().p())/(1+sqrt(cell->info().p()))));
// 					if (j % 100 == 0) cout<<"cell->info().p() "<<cell->info().p()<<" vs. "<< (- (cell->info().dv() - m) / (n) - cell->info().p())* relax<<endl;
					#endif
				}
                                dp -= cell->info().p();
                                #ifdef GS_OPEN_MP
                                const int tn=omp_get_thread_num();
				t_sum_dp[tn] += std::abs(dp);
				t_dp_max[tn]=max(t_dp_max[tn], std::abs(dp));
				t_p_max[tn]= max(t_p_max[tn], std::abs(cell->info().p()));
				t_sum_p[tn]+= std::abs(cell->info().p());
				#else
                                dp_max = max(dp_max, std::abs(dp));
                                p_max = max(p_max, std::abs(cell->info().p()));
                                sum_p += std::abs(cell->info().p());
                                sum_dp += std::abs(dp);
				#endif
                        }
                }
                #ifdef GS_OPEN_MP

                for (int ii=0;ii<num_threads;ii++) p_max =max(p_max, t_p_max[ii]);
		for (int ii=0;ii<num_threads;ii++) dp_max =max(dp_max, t_dp_max[ii]);
		for (int ii=0;ii<num_threads;ii++) sum_p+=t_sum_p[ii];
                for (int ii=0;ii<num_threads;ii++) sum_dp+=t_sum_dp[ii];
//                 cerr<< p_max<< " "<<dp_max<<" "<<sum_p<<" "<<sum_dp<<endl;
                #endif
		p_moy = sum_p/cell2;
 
		#ifdef GS_OPEN_MP
		#pragma omp master
		#endif
		j++;
	#ifdef GS_OPEN_MP
	} while (j<1500);
	#else
	} while ((dp_max/p_max) > tolerance /*&& j<4000*/ /*&& ( dp_max > tolerance )*//* &&*/ /*( j<50 )*/);
	#endif
	}

        if (DEBUG_OUT) {cout << "pmax " << p_max << "; pmoy : " << p_moy << endl;
        cout << "iteration " << j <<"; erreur : " << dp_max/p_max << endl;}
// 	iter << j << " " << dp_max/p_max << endl;
	computedOnce=true;
}

template <class Tesselation>
double FlowBoundingSphere<Tesselation>::boundaryFlux(unsigned int boundaryId)
{
	RTriangulation& Tri = T[currentTes].Triangulation();
	double Q1=0;

	Vector_Cell tmp_cells;
	tmp_cells.resize(10000);
	VCell_iterator cells_it = tmp_cells.begin();

	VCell_iterator cell_up_end = Tri.incident_cells(T[currentTes].vertexHandles[boundaryId],cells_it);
	for (VCell_iterator it = tmp_cells.begin(); it != cell_up_end; it++)
	{
		const Cell_handle& cell = *it;
		if (cell->info().isGhost) continue;
		for (int j2=0; j2<4; j2++)
			Q1 += (cell->neighbor(j2)->info().k_norm())[Tri.mirror_index(cell, j2)]* (cell->neighbor(j2)->info().p()-cell->info().p());
	}
	return Q1;
}

template <class Tesselation> 
double FlowBoundingSphere<Tesselation>::Permeameter(double P_Inf, double P_Sup, double Section, double DeltaY, const char *file)
{
  RTriangulation& Tri = T[currentTes].Triangulation();
  std::ofstream kFile(file, std::ios::out);
  double Q2=0, Q1=0;
  int cellQ1=0, cellQ2=0;
  double p_out_max=-10000000, p_out_min=10000000, p_in_max=-100000000, p_in_min=10000000,p_out_moy=0, p_in_moy=0;

  Vector_Cell tmp_cells;
  tmp_cells.resize(10000);
  VCell_iterator cells_it = tmp_cells.begin();

  VCell_iterator cell_up_end = Tri.incident_cells(T[currentTes].vertexHandles[y_max_id],cells_it);
  for (VCell_iterator it = tmp_cells.begin(); it != cell_up_end; it++)
  {
    Cell_handle& cell = *it;
    for (int j2=0; j2<4; j2++) {
      if (!cell->neighbor(j2)->info().Pcondition){
	Q1 = Q1 + (cell->neighbor(j2)->info().k_norm())[Tri.mirror_index(cell, j2)]* (cell->neighbor(j2)->info().p()-cell->info().p());
	cellQ1+=1;
	p_out_max = max(cell->neighbor(j2)->info().p(), p_out_max);
	p_out_min = min(cell->neighbor(j2)->info().p(), p_out_min);
	p_out_moy += cell->neighbor(j2)->info().p();}
  }}

  Vector_Cell tmp_cells2;
  tmp_cells2.resize(10000);
  VCell_iterator cells_it2 = tmp_cells2.begin();

  VCell_iterator cell_down_end = Tri.incident_cells(T[currentTes].vertexHandles[y_min_id],cells_it2);
  for (VCell_iterator it = tmp_cells2.begin(); it != cell_down_end; it++)
  {
    Cell_handle& cell = *it;
    for (int j2=0; j2<4; j2++){
      if (!cell->neighbor(j2)->info().Pcondition){
	Q2 = Q2 + (cell->neighbor(j2)->info().k_norm())[Tri.mirror_index(cell, j2)]* (cell->info().p()-cell->neighbor(j2)->info().p());
	cellQ2+=1;
	p_in_max = max(cell->neighbor(j2)->info().p(), p_in_max);
	p_in_min = min(cell->neighbor(j2)->info().p(), p_in_min);
	p_in_moy += cell->neighbor(j2)->info().p();}
  }}

	double density = 1;
        double viscosity = VISCOSITY;
        double gravity = 1;
        double Vdarcy = Q1/Section;
	double DeltaP = abs(P_Inf-P_Sup);
	double DeltaH = DeltaP/ (density*gravity);
	double k = viscosity*Vdarcy*DeltaY / DeltaP; /**m²**/
	double Ks = k*(density*gravity)/viscosity; /**m/s**/
	
	if (DEBUG_OUT){
	cout << "the maximum superior pressure is = " << p_out_max << " the min is = " << p_out_min << endl;
	cout << "the maximum inferior pressure is = " << p_in_max << " the min is = " << p_in_min << endl;
	cout << "superior average pressure is " << p_out_moy/cellQ1 << endl;
        cout << "inferior average pressure is " << p_in_moy/cellQ2 << endl;
        cout << "celle comunicanti in basso = " << cellQ2 << endl;
        cout << "celle comunicanti in alto = " << cellQ1 << endl;
        cout << "The incoming average flow rate is = " << Q2 << " m^3/s " << endl;
        cout << "The outgoing average flow rate is = " << Q1 << " m^3/s " << endl;
        cout << "The gradient of charge is = " << DeltaH/DeltaY << " [-]" << endl;
        cout << "Darcy's velocity is = " << Vdarcy << " m/s" <<endl;
        cout << "The permeability of the sample is = " << k << " m^2" <<endl;
	cout << endl << "The hydraulic conductivity of the sample is = " << Ks << " m/s" << endl << endl;
	}
	kFile << "y_max id = "<<y_max_id<< "y_min id = "<<y_min_id<<endl;
	kFile << "the maximum superior pressure is = " << p_out_max << " the min is = " << p_out_min << endl;
	kFile << "the maximum inferior pressure is = " << p_in_max << " the min is = " << p_in_min << endl;
        kFile << "superior average pressure is " << p_out_moy/cellQ2 << endl;
        kFile << "inferior average pressure is " << p_in_moy/cellQ1 << endl;
        kFile << "celle comunicanti in basso = " << cellQ2 << endl;
        kFile << "celle comunicanti in alto = " << cellQ1 << endl;
	kFile << "The incoming average flow rate is = " << Q2 << " m^3/s " << endl;
        kFile << "The outgoing average flow rate is = " << Q1 << " m^3/s " << endl;
        kFile << "The gradient of charge is = " << DeltaH/DeltaY << " [-]" << endl;
        kFile << "Darcy's velocity is = " << Vdarcy << " m/s" <<endl;
        kFile << "The hydraulic conductivity of the sample is = " << Ks << " m/s" <<endl;
        kFile << "The permeability of the sample is = " << k << " m^2" <<endl;
	return k;
}
template <class Tesselation> 
void FlowBoundingSphere<Tesselation>::DisplayStatistics()
{
	RTriangulation& Tri = T[currentTes].Triangulation();
        int Zero =0, Inside=0, Fictious=0;
        Finite_cells_iterator cell_end = Tri.finite_cells_end();
        for (Finite_cells_iterator cell = Tri.finite_cells_begin(); cell != cell_end; cell++) {
                int zeros =0;
                for (int j=0; j!=4; j++) {
                        if ((cell->info().k_norm())[j]==0) {
                                zeros+=1;
                        }
                }
                if (zeros==4) {
                        Zero+=1;
                }
                if (!cell->info().fictious()) {
                        Inside+=1;
                } else {
                        Fictious+=1;
                }
        }
        int fict=0, real=0;
        for (Finite_vertices_iterator v = Tri.finite_vertices_begin(); v != Tri.finite_vertices_end(); ++v) {
                if (v->info().isFictious) fict+=1;
                else real+=1;
        }
        long Vertices = Tri.number_of_vertices();
	long Cells = Tri.number_of_finite_cells();
	long Facets = Tri.number_of_finite_facets();
        if(DEBUG_OUT) {cout << "zeros = " << Zero << endl;
	cout << "There are " << Vertices << " vertices, dont " << fict << " fictious et " << real << " reeeeeel" << std::endl;
        cout << "There are " << Cells << " cells " << std::endl;
        cout << "There are " << Facets << " facets " << std::endl;
        cout << "There are " << Inside << " cells INSIDE." << endl;
        cout << "There are " << Fictious << " cells FICTIOUS." << endl;}

	num_particles = real;
}
template <class Tesselation> 
void FlowBoundingSphere<Tesselation>::saveVtk()
{
	RTriangulation& Tri = T[noCache?(!currentTes):currentTes].Triangulation();
        static unsigned int number = 0;
        char filename[80];
	mkdir("./VTK", S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
        sprintf(filename,"./VTK/out_%d.vtk", number++);
	int firstReal=-1;

	//count fictious vertices and cells
	vtk_infinite_vertices=vtk_infinite_cells=0;
 	Finite_cells_iterator cell_end = Tri.finite_cells_end();
        for (Finite_cells_iterator cell = Tri.finite_cells_begin(); cell != cell_end; cell++) {
		bool isDrawable = cell->info().isReal() && cell->vertex(0)->info().isReal() && cell->vertex(1)->info().isReal() && cell->vertex(2)->info().isReal()  && cell->vertex(3)->info().isReal();
		if (!isDrawable) vtk_infinite_cells+=1;
	}
	for (Finite_vertices_iterator v = Tri.finite_vertices_begin(); v != Tri.finite_vertices_end(); ++v) {
                if (!v->info().isReal()) vtk_infinite_vertices+=1;
                else if (firstReal==-1) firstReal=vtk_infinite_vertices;}

        basicVTKwritter vtkfile((unsigned int) Tri.number_of_vertices()-vtk_infinite_vertices, (unsigned int) Tri.number_of_finite_cells()-vtk_infinite_cells);

        vtkfile.open(filename,"test");

        vtkfile.begin_vertices();
        double x,y,z;
        for (Finite_vertices_iterator v = Tri.finite_vertices_begin(); v != Tri.finite_vertices_end(); ++v) {
		if (v->info().isReal()){
		x = (double)(v->point().point()[0]);
                y = (double)(v->point().point()[1]);
                z = (double)(v->point().point()[2]);
                vtkfile.write_point(x,y,z);}
        }
        vtkfile.end_vertices();

        vtkfile.begin_cells();
        for (Finite_cells_iterator cell = Tri.finite_cells_begin(); cell != Tri.finite_cells_end(); ++cell) {
		bool isDrawable = cell->info().isReal() && cell->vertex(0)->info().isReal() && cell->vertex(1)->info().isReal() && cell->vertex(2)->info().isReal()  && cell->vertex(3)->info().isReal();
        	if (isDrawable){vtkfile.write_cell(cell->vertex(0)->info().id()-firstReal, cell->vertex(1)->info().id()-firstReal, cell->vertex(2)->info().id()-firstReal, cell->vertex(3)->info().id()-firstReal);}
        }
        vtkfile.end_cells();

	if (permeability_map){
	vtkfile.begin_data("Permeability",CELL_DATA,SCALARS,FLOAT);
	for (Finite_cells_iterator cell = Tri.finite_cells_begin(); cell != Tri.finite_cells_end(); ++cell) {
		bool isDrawable = cell->info().isReal() && cell->vertex(0)->info().isReal() && cell->vertex(1)->info().isReal() && cell->vertex(2)->info().isReal()  && cell->vertex(3)->info().isReal();
		if (isDrawable){vtkfile.write_data(cell->info().s);}
	}
	vtkfile.end_data();}
	else{
	vtkfile.begin_data("Pressure",CELL_DATA,SCALARS,FLOAT);
	for (Finite_cells_iterator cell = Tri.finite_cells_begin(); cell != Tri.finite_cells_end(); ++cell) {
		bool isDrawable = cell->info().isReal() && cell->vertex(0)->info().isReal() && cell->vertex(1)->info().isReal() && cell->vertex(2)->info().isReal()  && cell->vertex(3)->info().isReal();
		if (isDrawable){vtkfile.write_data(cell->info().p());}
	}
	vtkfile.end_data();}

	if (1){
	Average_Relative_Cell_Velocity();
	vtkfile.begin_data("Velocity",CELL_DATA,VECTORS,FLOAT);
	for (Finite_cells_iterator cell = Tri.finite_cells_begin(); cell != Tri.finite_cells_end(); ++cell) {
		bool isDrawable = cell->info().isReal() && cell->vertex(0)->info().isReal() && cell->vertex(1)->info().isReal() && cell->vertex(2)->info().isReal()  && cell->vertex(3)->info().isReal();
		if (isDrawable){vtkfile.write_data(cell->info().av_vel()[0],cell->info().av_vel()[1],cell->info().av_vel()[2]);}
	}
	vtkfile.end_data();}
}

#ifdef XVIEW
template <class Tesselation> 
void FlowBoundingSphere<Tesselation>::Dessine_Triangulation(Vue3D &Vue, RTriangulation &T)
{
        double* Segments = NULL;
        long N_seg = New_liste_edges(T, &Segments);
        Vue.Dessine_Segment(Segments, N_seg);
        Delete_liste_edges(&Segments, N_seg);
}
template <class Tesselation> 
void FlowBoundingSphere<Tesselation>::Dessine_Short_Tesselation(Vue3D &Vue, Tesselation &Tes)
{
        if (!Tes.Computed()) Tes.Compute();
        double* Segments = NULL;
        long N_seg = Tes.New_liste_short_edges(&Segments);
        Vue.Dessine_Segment(Segments, N_seg);
        Delete_liste_edges(&Segments, N_seg);
}
#endif
template <class Tesselation> 
void FlowBoundingSphere<Tesselation>::GenerateVoxelFile( )
{
	RTriangulation& Tri = T[currentTes].Triangulation();
        double l = 1;
        int dx = 200;
        double eps = l/dx;

        std::ofstream voxelfile("MATRIX",std::ios::out);
        bool solid=false;

        for (double y=0; y<=l; y+=eps) {
                for (double z=0; z<=l; z+=eps) {
                        for (double x=0; x<=l; x+=eps) {
                                solid=false;

                                for (Finite_vertices_iterator V_it = Tri.finite_vertices_begin(); V_it != Tri.finite_vertices_end(); V_it++) {
                                        double radius = sqrt(V_it->point().weight());
                                        if ((sqrt(pow((x- (V_it->point()[0])),2) +pow((y- (V_it->point()[1])),2) +pow((z- (V_it->point()[2])),2))) <= radius) solid=true;
                                }
                                if (solid) voxelfile << 1;
                                else voxelfile << 0;
                        }
                        voxelfile << endl;
                }
        }
}

template <class Tesselation>
double FlowBoundingSphere<Tesselation>::Sample_Permeability(double& x_Min,double& x_Max ,double& y_Min,double& y_Max,double& z_Min,double& z_Max/*, string key*/)
{
        double Section = (x_Max-x_Min) * (z_Max-z_Min);
        double DeltaY = y_Max-y_Min;
        boundary(y_min_id).flowCondition=0;
        boundary(y_max_id).flowCondition=0;
        boundary(y_min_id).value=0;
        boundary(y_max_id).value=1;
	double P_zero = abs((boundary(y_min_id).value-boundary(y_max_id).value)/2);
	Initialize_pressures( P_zero );
	GaussSeidel();
	const char *kk = "Permeability";
        return Permeameter(boundary(y_min_id).value, boundary(y_max_id).value, Section, DeltaY, kk);
}
template <class Tesselation> 
bool FlowBoundingSphere<Tesselation>::isInsideSphere(double& x, double& y, double& z)
{
	RTriangulation& Tri = T[currentTes].Triangulation();
        for (Finite_vertices_iterator V_it = Tri.finite_vertices_begin(); V_it != Tri.finite_vertices_end(); V_it++) {
                double radius = V_it->point().weight();
                if (pow((x- (V_it->point()[0])),2) +pow((y- (V_it->point()[1])),2) +pow((z- (V_it->point()[2])),2)   <= radius) return true;
        }
        return false;
}
template <class Tesselation> 
void FlowBoundingSphere<Tesselation>::SliceField(const char *filename)
{
        /** Pressure field along one cutting plane **/
	RTriangulation& Tri = T[noCache?(!currentTes):currentTes].Triangulation();
        Cell_handle permeameter;

        std::ofstream consFile(filename,std::ios::out);

        int intervals = 400;
        double Ry = (y_max-y_min) /intervals;
        double Rz = (z_max-z_min) /intervals;
//         cout<<Rx<<" "<<Ry<<" "<<Rz<<" "<<z_max<<" "<<z_min<<" "<<y_max<<" "<<y_min<<" "<<x_max<<" "<<x_min<<endl;

//         for (double X=min(x_min,x_max); X<=max(x_min,x_max); X=X+abs(Rx)) {
	double X=0.5;
                for (double Y=min(y_max,y_min); Y<=max(y_max,y_min); Y=Y+abs(Ry)) {
                        for (double Z=min(z_min,z_max); Z<=max(z_min,z_max); Z=Z+abs(Rz)) {
			  permeameter = Tri.locate(Point(X, Y, Z));
			  consFile << permeameter->info().p() <<" ";
//                                 if (!isInsideSphere(X,Y,Z)) {
//                                         permeameter = Tri.locate(Point(X, Y, Z));
//                                         consFile << permeameter->info().p() <<" ";
//                                         //cout <<"valeur trouvée";
//                                 } else consFile << "Nan ";
                        }
                        consFile << endl;}
                consFile << endl;
//         }
        consFile.close();
}
template <class Tesselation> 
void FlowBoundingSphere<Tesselation>::ComsolField()
{
	//Compute av. velocity first, because in the following permeabilities will be overwritten with "junk" (in fact velocities from comsol)
	Average_Relative_Cell_Velocity();

  	RTriangulation& Tri = T[noCache?(!currentTes):currentTes].Triangulation();
        Cell_handle c;
  	ifstream loadFile("vx_grid_03_07_ns.txt");
	ifstream loadFileY("vy_grid_03_07_ns.txt");
	ifstream loadFileZ("vz_grid_03_07_ns.txt");
	int Nx=100; int Ny=10; int Nz=100;
	std::ofstream consFile("velComp",std::ios::out);

	Finite_cells_iterator cell_end = Tri.finite_cells_end();
	for (Finite_cells_iterator cell = Tri.finite_cells_begin(); cell != cell_end; cell++){
		cell->info().dv()=0;
		cell->info().module_permeability[0]=cell->info().module_permeability[1]=cell->info().module_permeability[2]=0;
	}

	vector<Real> X, Y, Z;
	Real buffer;
	for (int xi=0;xi<Nx;xi++) {loadFile >> buffer; X.push_back(buffer); loadFileY >> buffer; loadFileZ >> buffer;}
	for (int yi=0;yi<Ny;yi++) {loadFile >> buffer; Y.push_back(buffer); loadFileY >> buffer; loadFileZ >> buffer;}
	for (int zi=0;zi<Nz;zi++) {loadFile >> buffer; Z.push_back(buffer); loadFileY >> buffer; loadFileZ >> buffer;}

	Real vx, vy, vz;
	Real meanCmsVel=0; int totCmsPoints = 0;
	for (int zi=0;zi<Nz;zi++)
	  	for (int yi=0;yi<Ny;yi++)
		  	for (int xi=0;xi<Nx;xi++) {
			  	loadFile >> vx; loadFileY >> vy; loadFileZ >> vz;
				if (!isInsideSphere(X[xi], Y[yi], Z[zi]) && vx!=0) {
                                        c = Tri.locate(Point(X[xi], Y[yi], Z[zi]));
                                        c->info().module_permeability[0]+=vx;
					c->info().module_permeability[1]+=vy;
					c->info().module_permeability[2]+=vz;
					c->info().dv()+=1;
					meanCmsVel+=vy; totCmsPoints++;}
	}
	int kk=0;
	Vecteur diff;
	for (Finite_cells_iterator cell = Tri.finite_cells_begin(); cell != cell_end && kk<10000; cell++){
		if (cell->info().fictious() || cell->info().dv()<60) continue;
		for (int k=0;k<3;k++) cell->info().module_permeability[k]/=cell->info().dv();
		cerr << cell->info().module_permeability[0]<<" "<< cell->info().module_permeability[1]<<" "<< cell->info().module_permeability[2]<<" "<<cell->info().dv()<<" "<< cell->info().av_vel()<<endl;
		Real m=sqrt(pow(cell->info().module_permeability[0],2)+pow(cell->info().module_permeability[1],2)+pow(cell->info().module_permeability[2],2));
		Vecteur comFlow (cell->info().module_permeability[0],cell->info().module_permeability[1],cell->info().module_permeability[2]);
		Real angle=asin(sqrt(cross_product(comFlow,cell->info().av_vel()).squared_length())/(sqrt(comFlow.squared_length())*sqrt(cell->info().av_vel().squared_length())));
		cerr<<"norms : "<<m<<" vs. "<<sqrt(cell->info().av_vel().squared_length())<<" angle "<<180*angle/3.1415<<endl;
		consFile<<m<<" "<<sqrt(cell->info().av_vel().squared_length())<<" "<<180*angle/3.1415<<endl;
		diff = diff + (comFlow - cell->info().av_vel());
		kk++;
	}
	cerr << "meanCmsVel "<<meanCmsVel/totCmsPoints<<" mean diff "<<diff/kk<<endl;
}
template <class Tesselation>
void  FlowBoundingSphere<Tesselation>::computeEdgesSurfaces()
{
  RTriangulation& Tri = T[currentTes].Triangulation();

  //first, copy interacting pairs and normal lub forces form prev. triangulation in a sorted structure for initializing the new lub. Forces
  vector<vector<pair<unsigned int,Real> > > lubPairs;
  lubPairs.resize(Tri.number_of_vertices()+1);
  for (unsigned int k=0; k<edgeNormalLubF.size(); k++)
	lubPairs[min(Edge_ids[k].first,Edge_ids[k].second)].push_back(pair<int,Real> (max(Edge_ids[k].first,Edge_ids[k].second),edgeNormalLubF[k]));

  //Now we reset the containers and initialize them
  Edge_Surfaces.clear(); Edge_ids.clear(); edgeNormalLubF.clear();
  Finite_edges_iterator ed_it;
  for ( Finite_edges_iterator ed_it = Tri.finite_edges_begin(); ed_it!=Tri.finite_edges_end();ed_it++ )
  {
    int hasFictious= (ed_it->first)->vertex(ed_it->second)->info().isFictious +  (ed_it->first)->vertex(ed_it->third)->info().isFictious;
    if (hasFictious==2) continue;
    double area = T[currentTes].ComputeVFacetArea(ed_it);
    Edge_Surfaces.push_back(area);
    unsigned int id1 = (ed_it->first)->vertex(ed_it->second)->info().id();
    unsigned int id2 = (ed_it->first)->vertex(ed_it->third)->info().id();
    Edge_ids.push_back(pair<int,int>(id1,id2));
    
    //For persistant edges, we must transfer the lub. force value from the older triangulation structure
    if (id1>id2) swap(id1,id2);
    unsigned int i=0;
    //Look for the pair (id1,id2) in lubPairs
    while (i<lubPairs[id1].size()) {
		if (lubPairs[id1][i].first == id2) {
			//it's found, we copy the lub force
			edgeNormalLubF.push_back(lubPairs[id1][i].second);
			break;}
		++i;
    }
    // not found, we initialize with zero lub force
    if (i==lubPairs[id1].size()) edgeNormalLubF.push_back(0);
  }
}

template <class Tesselation> 
Vector3r FlowBoundingSphere<Tesselation>::computeViscousShearForce(const Vector3r& deltaV, const int& edge_id, const Real& Rh)
{
    Vector3r tau = deltaV*VISCOSITY/Rh;
    return tau * Edge_Surfaces[edge_id];
}

template <class Tesselation> 
Vector3r FlowBoundingSphere<Tesselation>::computeShearLubricationForce(const Vector3r& deltaShearV, const Real& dist, const int& edge_id, const Real& eps, const Real& centerDist, const Real& meanRad )
{
    Real d = max(dist,0.) + eps*meanRad;
    Vector3r viscLubF = 0.5 * Mathr::PI * VISCOSITY * (-2*meanRad + centerDist*log(centerDist/d)) * deltaShearV;
    return viscLubF;
}

template <class Tesselation> 
Real FlowBoundingSphere<Tesselation>::computeNormalLubricationForce(const Real& deltaNormV, const Real& dist, const int& edge_id, const Real& eps, const Real& stiffness, const Real& dt, const Real& meanRad)
{
	//FIXME: here introduce elasticity
	Real d = max(dist,0.) + eps*meanRad;//account for grains roughness
	if (stiffness>0) {
		const Real k = stiffness*meanRad;
		Real prevForce = edgeNormalLubF[edge_id];
// 		Real prevForce = edgeNormalLubF[edge_id] ? edgeNormalLubF[edge_id] : (6*Mathr::PI*pow(meanRad,2)* VISCOSITY* deltaNormV)/d;
// 		if (!edgeNormalLubF[edge_id]) for (int kk=0; kk<30; kk++) {
// 			Real instantVisc = 6*Mathr::PI*pow(meanRad,2)*VISCOSITY/(d-prevForce/k);
// 			prevForce = instantVisc*(deltaNormV + prevForce/(k*dt))/(1+instantVisc/(k*dt));
// 			if ((kk==0 || kk==29) && deltaNormV!=0) cerr << "prevForce("<<kk<<") = "<< prevForce<<endl;
// 		}
		Real instantVisc = 1.5*Mathr::PI*pow(meanRad,2)*VISCOSITY/(d-prevForce/k);
		Real normLubF = instantVisc*(deltaNormV + prevForce/(k*dt))/(1+instantVisc/(k*dt));
		edgeNormalLubF[edge_id]=normLubF;
		return normLubF;
	} else {
		Real normLubF = (1.5*Mathr::PI*pow(meanRad,2)* VISCOSITY* deltaNormV)/d;
		return normLubF;
	}
}

} //namespace CGT

#endif //FLOW_ENGINE

