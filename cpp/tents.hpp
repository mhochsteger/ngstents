#ifndef TENTSHEADER
#define TENTSHEADER

#include <solve.hpp>
using namespace ngsolve;
using namespace std;

// A spacetime tent is a macroelement consisting of a tentpole erected at
// a central vertex in space and all the space-time tetrahedra with
// the tentpole as an edge.

// We represent the tent by its projection on space (a vertex patch),
// the central vertex, and the heights (times) of its neighboring
// vertices.

////////////////////////////////////////////////////////////////////////////

// Class with tent geometry information:

class Tent {

public:

  int vertex;                 // central vertex
  double tbot, ttop;          // bottom and top times of central vertex
  Array<int> nbv;             // neighbour vertices
  Array<double> nbtime;       // height/time of neighbouring vertices
  Array<int> els;             // all elements in the tent's vertex patch
  Array<int> internal_facets; // all internal facets in the tent's vertex patch
  Table<int> elfnums;         /* elfnums[k] lists all internal facets of
				 the k-th element of tent */

  // tent top and bottom are graphs of p.w.linear phi_top, phi_bot
  Array<Matrix<>> gradphi_bot, gradphi_top;
  Array<Vector<double>> delta; // phi_top - phi_bot
  Array<Vector<>> graddelta;
  Table<Matrix<>> gradphi_facet_bot, gradphi_facet_top;
  Table<Vector<double>> delta_facet;

  // access to global periodicity identifications
  static Array<int> vmap;      // vertex map for periodic spaces
  
  // access to the finite element & dofs
  class TentDataFE * tempdata = nullptr;

  // other global details from a mesh of tents
  int level;                   // parallel layer number
  Array<int> dependent_tents;  // these tents depend on me

};

void VTKOutputTents(shared_ptr<MeshAccess> maptr, Array<Tent*> & tents,
                    string filename);
ostream & operator<< (ostream & ost, const Tent & tent);


////////////////////////////////////////////////////////////////////////////

// Class with dofs, finite element & integration info for a tent:

class TentDataFE
{
public:
  // ELEMENT DATA
  // finite elements for all elements in the tent
  Array<FiniteElement*> fei;
  // integration rules for all elements in the tent
  Array<SIMD_IntegrationRule*> iri;
  // mapped integration rules for all elements in the tent
  Array<SIMD_BaseMappedIntegrationRule*> miri;
  // element transformations for all elements in the tent
  Array<ElementTransformation*> trafoi;
  // mesh size for each element
  Array<double> mesh_size;
  // gradient of the old advancing front in the IP's
  Array<FlatMatrix<SIMD<double>>> agradphi_bot;
  // gradient of the new advancing front in the IP's
  Array<FlatMatrix<SIMD<double>>> agradphi_top;
  // height of the tent in the IP's
  Array<FlatVector<SIMD<double>>> adelta;

  // FACET DATA
  // local numbers of the neighbors
  Array<INT<2,size_t>> felpos;
  // facet integration rules for all facets in the tent
  // transformed to local coordinated of the neighboring elements
  Array<Vec<2,const SIMD_IntegrationRule*>> firi;
  // mapped facet integration rules for all facets
  Array<SIMD_BaseMappedIntegrationRule*> mfiri1;
  // mapped facet integration rules for all facets
  Array<SIMD_BaseMappedIntegrationRule*> mfiri2;
  // gradient phi face first and second element
  Array<FlatMatrix<SIMD<double>>> agradphi_botf1;
  Array<FlatMatrix<SIMD<double>>> agradphi_topf1;
  Array<FlatMatrix<SIMD<double>>> agradphi_botf2;
  Array<FlatMatrix<SIMD<double>>> agradphi_topf2;
  // normal vectors in the IP's
  Array<FlatMatrix<SIMD<double>>> anormals;
  // height of the tent in the IP's
  Array<FlatVector<SIMD<double>>> adelta_facet;

  // first constructor just initializes arrays
  TentDataFE (int n, LocalHeap & lh)
    : fei(n, lh), iri(n, lh), miri(n, lh), trafoi(n, lh) { ; }

  // second constructor performs all initialization
  TentDataFE (const Tent & tent, const FESpace & fes,
                const MeshAccess & ma, LocalHeap & lh);
};


////////////////////////////////////////////////////////////////////////////

template <int DIM>
class TentPitchedSlab {

public:

  Array<Tent*> tents;         // tents between two time slices
  double dt;                  // time step between two time slices
  Table<int> tent_dependency; // DAG of tent dependencies
  shared_ptr<MeshAccess> ma;  // access to base spatial mesh   

  TentPitchedSlab(shared_ptr<MeshAccess> ama) : ma(ama) { ; };

  // Construct tentpitched mesh of slab and tent dependencies
  void PitchTents(double dt, double cmax, LocalHeap & lh);
  void PitchTents(double dt, shared_ptr<CoefficientFunction> cmax, LocalHeap & lh);

  // Draw pitched tents 
  void DrawPitchedTents(int level=1) ; 

  // Draw pitched tents into a VTK output file  
  void DrawPitchedTentsVTK(string vtkfilename);

};


#endif