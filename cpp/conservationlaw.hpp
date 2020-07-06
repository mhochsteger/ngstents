#ifndef CONSERVATIONLAW_HPP
#define CONSERVATIONLAW_HPP

#ifdef USE_NETGEN_GUI
#include "myvisual.hpp"
#endif
#include "tents.hpp"
#include <atomic>

class ConservationLaw
{
public:
  string equation = "";
  shared_ptr<MeshAccess> ma = NULL;
  shared_ptr<L2HighOrderFESpace> fes = NULL;
  shared_ptr<GridFunction> gfu = NULL;
  shared_ptr<GridFunction> gfres = NULL;
  shared_ptr<GridFunction> gfuorig = NULL;
  shared_ptr<GridFunction> gfnu = NULL;
  shared_ptr<LocalHeap> pylh;

  AutoVector u;     // u(n)
  AutoVector uinit; // initial data, also used for bc
  AutoVector flux;

  // advancing front (used for time-dependent bc)
  shared_ptr<GridFunction> gftau = nullptr;

public:
  ConservationLaw (shared_ptr<MeshAccess> ama) { ma = ama; };

  virtual ~ConservationLaw() { ; }

  virtual string Equation() = 0;

  virtual void CheckBC() = 0;

  ///////////////////// tent pitching functions ///////////////////////////////

  virtual int GetNTents() = 0;

  virtual void PitchTents(double adt,
                          shared_ptr<CoefficientFunction> awavespeed) = 0;

  virtual double MaxSlope();

  void DrawPitchedTentsVTK(string vtkfilename);

  void DrawPitchedTentsGL(Array<int> & tentdata,
                          Array<double> & tenttimes, int & nlevels);

  virtual void PropagatePicard(int steps, BaseVector & hu,
                               BaseVector & hu0, LocalHeap & lh) = 0;

};


////////////////////// T_ConservationLaw ///////////////////////////

template <typename EQUATION, int DIM, int COMP, int ECOMP, bool XDEPENDENT>
class T_ConservationLaw : public ConservationLaw
{
protected:
  FlatVector<> nu;  // viscosity coefficient

  bool def_bcnr = false; // check if the array below is properly set
  int maxbcnr = 4;
  Array<int> bcnr; // array of boundary condition numbers

  // dt=timeslab height for tentpitching, timestep for other methods
  double dt;
  double tend;      // tend=final time

  // collection of tents in timeslab
  size_t tentslab_heapsize = 10*1000000;
  TentPitchedSlab<DIM> tps = TentPitchedSlab<DIM>(ma, tentslab_heapsize);
  
  Table<int> & tent_dependency = tps.tent_dependency;

  double wavespeed;

  Array<IntegrationRule*> glrules;

  const EQUATION & Cast() const {return static_cast<const EQUATION&> (*this);}

public:
  T_ConservationLaw (shared_ptr<MeshAccess> ama, int order, const Flags & flags)
    : ConservationLaw (ama)
  {
    size_t heapsize = 10*1000000;
    pylh = make_shared<LocalHeap>(heapsize,"ConsLaw - py main heap",true);

    Init(flags);
    // store boundary condition numbers
    bcnr = FlatArray<int>(ama->GetNFacets(),*pylh);
    bcnr = -1;

    // Main L2 finite element space based on spatial mesh
    Flags fesflags = Flags();
    fesflags.SetFlag("order",order);
    fesflags.SetFlag("dim",COMP);
    fesflags.SetFlag("all_dofs_together");
    fes = dynamic_pointer_cast<L2HighOrderFESpace>(
        CreateFESpace("l2ho", ma, fesflags));
    fes->Update();
    fes->FinalizeUpdate();

    gfu = CreateGridFunction(fes,"u",Flags());
    gfu->Update();

    if (ECOMP > 0)
      {
        // Scalar L2 finite element space for entropy residual
        shared_ptr<FESpace> fes_scal =
          CreateFESpace("l2ho", ma,
              Flags().SetFlag("order",order).SetFlag("all_dofs_together"));
        fes_scal->Update();
        fes_scal->FinalizeUpdate();
        gfres = CreateGridFunction(fes_scal,"res",Flags());
	gfres->Update();

        // Zero order L2 finite element space for viscosity
	shared_ptr<FESpace> fes_lo = CreateFESpace("l2ho", ma,
                                                   Flags().SetFlag("order",0));
	fes_lo->Update();
	fes_lo->FinalizeUpdate();
	gfnu = CreateGridFunction(fes_lo,"nu",Flags());
	gfnu->Update();
      }
    gfuorig = CreateGridFunction(fes,"uorig",Flags());
    gfuorig->Update();

    // first order H1 space for the advancing front
    shared_ptr<FESpace> fesh1 = CreateFESpace("h1ho", ma,
                                              Flags().SetFlag("order",1));
    fesh1->Update();
    fesh1->FinalizeUpdate();
    gftau = CreateGridFunction(fesh1,"tau",Flags().SetFlag("novisual"));
    gftau->Update();
    gftau->GetVector() = 0.0;

    AllocateVectors();
  }

  void Init(const Flags & flags) {
    dt = flags.GetNumFlag ("dt", 1e-3);
    tend = flags.GetNumFlag ("tend", 1.0);

    wavespeed = flags.GetNumFlag ("wavespeed", 100.0);

    Array<double> xn, wn;
    for(int n = 2; 2*n-3 <= 10; n++)
      {
        ComputeGaussLobattoRule(n,xn,wn);
        IntegrationRule * intrule = new IntegrationRule;
        for(int i : Range(xn.Size()))
          intrule->Append(IntegrationPoint(xn[i], 0.0, 0.0, wn[i]));
        glrules.Append(intrule);
      }
  }

  void AllocateVectors()
  {
    u.AssignPointer(gfu->GetVectorPtr());
    uinit.AssignPointer(u.CreateVector());
    flux.AssignPointer(u.CreateVector());
    if(gfnu != NULL)
      {
	gfnu->Update();
	nu.AssignMemory(gfnu->GetVector().FVDouble().Size(),
                        &gfnu->GetVector().FVDouble()(0));
	nu = 0.0;
      }
  }

  virtual ~T_ConservationLaw()
  {
    for(auto intrule : glrules)
      delete intrule;
  }

  virtual string Equation() { return equation; }

  virtual void CheckBC()
  {
    if(!def_bcnr)
      for(int i : Range(ma->GetNSE()))
        {
          auto sel = ElementId(BND,i);
          auto fnums = ma->GetElFacets(sel);
          bcnr[fnums[0]] = ma->GetElIndex(sel);
        }
  }

  int GetNTents() { return tps.GetNTents(); }

  virtual void PitchTents(double adt, shared_ptr<CoefficientFunction> awavespeed)
  {
    tps.PitchTents(adt, awavespeed);
  }

  virtual double MaxSlope()
  {
    return tps.MaxSlope();
  }

  void DrawPitchedTentsVTK(string vtkfilename)
  {
    tps.DrawPitchedTentsVTK(vtkfilename);
  }

  void DrawPitchedTentsGL(Array<int> & tentdata,
                          Array<double> & tenttimes, int & nlevels)
  {
    tps.DrawPitchedTentsGL(tentdata, tenttimes, nlevels);
  }

  template <int W>
  void SolveM (const Tent & tent, int loci, FlatMatrixFixWidth<W> mat,
               LocalHeap & lh) const
  {
    auto fedata = tent.fedata;
    if (!fedata)
        throw Exception ("Expected tent.fedata to be set!");

    HeapReset hr(lh);
    const DGFiniteElement<DIM> & fel =
      static_cast<const DGFiniteElement<DIM>&> (*fedata->fei[loci]);

    bool curved = ma->GetElement(ElementId(VOL,tent.els[loci])).is_curved;
    if (curved)
      {
	FlatVector<> diagmass(mat.Height(),lh);
	fel.GetDiagMassMatrix(diagmass);

	SIMD_IntegrationRule & ir = *fedata->iri[loci];
	SIMD_BaseMappedIntegrationRule & mir = *fedata->miri[loci];
	FlatMatrix<SIMD<double>> pntvals(W, ir.Size(), lh);

	for (int i : Range(mat.Height()))
	  mat.Row(i) /= diagmass(i);
        fel.Evaluate(ir, mat, pntvals);
	for (int comp : Range(W))
	  {
            for(int i : Range(ir))
              pntvals(comp,i) *= ir[i].Weight() / mir[i].GetMeasure();
	  }
        mat = 0.0;
        fel.AddTrans(ir,pntvals, mat);
	for (int i : Range(mat.Height()))
	  mat.Row(i) /= diagmass(i);
      }

    else
      {
        FlatVector<> diagmass(mat.Height(),lh);

        double measure = (*fedata->miri[loci])[0].GetMeasure()[0];
        fel.GetDiagMassMatrix(diagmass);
        for (int j = 0; j < diagmass.Size(); j++)
          diagmass(j) = 1.0 / (diagmass(j) * measure);
        for(int j = 0; j < mat.Height(); j++)
          mat.Row (j) *= diagmass(j);
      }
  }


  template <int W>
  void SolveM (const Tent & tent, int loci,
               FlatVector<SIMD<double>> delta,
               FlatMatrixFixWidth<W> mat, LocalHeap & lh) const
  {
    auto fedata = tent.fedata;
    if (!fedata)
        throw Exception ("Expected tent.fedata to be set!");

    HeapReset hr(lh);
    const DGFiniteElement<DIM> & fel =
      static_cast<const DGFiniteElement<DIM>&> (*fedata->fei[loci]);

    FlatVector<> diagmass(mat.Height(),lh);
    fel.GetDiagMassMatrix(diagmass);

    SIMD_IntegrationRule & ir = *fedata->iri[loci];
    SIMD_BaseMappedIntegrationRule & mir = *fedata->miri[loci];
    FlatMatrix<SIMD<double>> pntvals(W, ir.Size(), lh);

    for (int i : Range(mat.Height()))
      mat.Row(i) /= diagmass(i);
    fel.Evaluate(ir, mat, pntvals);
    for (int comp : Range(W))
      {
        for(int i : Range(ir))
          pntvals(comp,i) *= ir[i].Weight() *
            delta(i) / mir[i].GetMeasure(); //scale with 1/delta
      }
    mat = 0.0;
    fel.AddTrans(ir,pntvals, mat);
    for (int i : Range(mat.Height()))
      mat.Row(i) /= diagmass(i);
  }

  template <typename SCAL>
  Mat<COMP,DIM,SCAL> Flux (const BaseMappedIntegrationPoint & mip,
                           const FlatVec<COMP,SCAL> & u) const
  {
    return Cast().Flux(u);
  }

  void Flux (const SIMD_BaseMappedIntegrationRule & mir,
             FlatMatrix<SIMD<double>> u, FlatMatrix<SIMD<double>> flux) const
  {
    throw Exception ("flux for FlatMatrix<SIMD> not implemented");
  }

  void Flux(SIMD_BaseMappedIntegrationRule & mir,
            FlatMatrix<SIMD<double>> ul, FlatMatrix<SIMD<double>> ur,
            FlatMatrix<SIMD<double>> normals, FlatMatrix<SIMD<double>> fna) const
  {
    if (!XDEPENDENT)
      Cast().Flux (ul, ur, normals, fna);
    else
      throw Exception ("simd-flux not implemented for X-dependent equation");
  }

  void Flux(FlatMatrix<SIMD<double>> ul, FlatMatrix<SIMD<double>> ur,
            FlatMatrix<SIMD<double>> normals, FlatMatrix<SIMD<double>> fna) const
  {
    throw Exception ("flux for FlatMatrix<SIMD> not implemented for boundary");
  }

  Vec<COMP> Flux (const BaseMappedIntegrationPoint & mip,
                  const FlatVec<COMP> & ul, const FlatVec<COMP> & ur,
                  const Vec<DIM> & nv) const
  {
    return Cast().Flux(ul,ur,nv);
  }

  template<typename SCAL=double>
  Vec<COMP,SCAL> Flux (const FlatVec<COMP,SCAL> & ul,
                       const FlatVec<COMP,SCAL> & ur,
                       const Vec<DIM,SCAL> & nv) const
  {
    throw Exception ("flux not implemented for boundary");
  }

  void u_reflect(FlatMatrix<SIMD<double>> u, FlatMatrix<SIMD<double>> normals,
                 FlatMatrix<SIMD<double>> u_refl) const
  {
    Cast().u_reflect(u,normals,u_refl);
  }

  void u_transparent(SIMD_BaseMappedIntegrationRule & mir,
                     FlatMatrix<SIMD<double>> u, FlatMatrix<SIMD<double>> normals,
                     FlatMatrix<SIMD<double>> u_transp) const
  {
    throw Exception ("Transparent boundary just available for wave equation!");
  }

  void EntropyFlux (const Vec<DIM+2> & ml, const Vec<DIM+2> & mr,
                    const Vec<DIM> & n, double & flux) const
  {
    cout << "no overload for EntropyFlux" << endl;
  }

  void EntropyFlux (FlatMatrix<SIMD<double>> ml, FlatMatrix<SIMD<double>> mr,
                    FlatMatrix<SIMD<double>> n,
                    FlatMatrix<SIMD<double>> flux) const
  {
    cout << "no overload for EntropyFlux for FlatMatrix<SIMD>" << endl;
  }

  void CalcFluxTent (int tentnr, FlatMatrixFixWidth<COMP> u,
                     FlatMatrixFixWidth<COMP> u0, FlatMatrixFixWidth<COMP> flux,
                     double tstar, LocalHeap & lh);

  void Cyl2Tent (int tentnr, FlatMatrixFixWidth<COMP> uhat,
                          FlatMatrixFixWidth<COMP> u, double tstar,
                          LocalHeap & lh);

  void CalcViscosityTent (int tentnr, FlatMatrixFixWidth<COMP> u,
                          FlatMatrixFixWidth<COMP> ubnd, FlatVector<double> nu,
                          FlatMatrixFixWidth<COMP> visc, LocalHeap & lh);

  void CalcEntropy(FlatMatrix<AutoDiff<1,SIMD<double>>> adu,
                   FlatMatrix<AutoDiff<1,SIMD<double>>> grad,
		   FlatMatrix<SIMD<double>> dEdt,
                   FlatMatrix<SIMD<double>> F) const
  {
    cout << "no overload for CalcEntropy for tent pitching" << endl;
  }

  void CalcEntropyResidualTent (int tentnr, FlatMatrixFixWidth<COMP> u,
                                FlatMatrixFixWidth<COMP> ut,
                                FlatMatrixFixWidth<ECOMP> res,
                                FlatMatrixFixWidth<COMP> u0, double tstar,
                                LocalHeap & lh);

  double CalcViscosityCoefficientTent (int tentnr, FlatMatrixFixWidth<COMP> u,
                                       FlatMatrixFixWidth<ECOMP> hres,
				       double tstar, LocalHeap & lh);

  void CalcViscCoeffEl(const SIMD_BaseMappedIntegrationRule & mir,
                       FlatMatrix<SIMD<double>> elu_ipts,
                       FlatMatrix<SIMD<double>> res_ipts,
                       const double hi, double & coeff) const
  {
    cout << "no overload for CalcViscCoeffEl with FlatMatrix<SIMD>" << endl;
  }

  void ApplyM1 (int tentnr, double tstar, FlatMatrixFixWidth<COMP> u,
                FlatMatrixFixWidth<COMP> res, LocalHeap & lh);

  void ApplyM (int tentnr, double tstar,
               FlatMatrixFixWidth<COMP> u, FlatMatrixFixWidth<COMP> res,
               LocalHeap & lh);

  void Tent2Cyl (int tentnr, FlatMatrixFixWidth<COMP> u,
                 FlatMatrixFixWidth<COMP> uhat,
                 double tstar, LocalHeap & lh);

  template <typename T = SIMD<double>>
  void TransformBackIR(const SIMD_BaseMappedIntegrationRule & mir,
                       FlatMatrix<T> grad, FlatMatrix<T> u) const
  {
    throw Exception ("TransformBack for FlatMatrix<SIMD> not available");
  }

  void PropagatePicard(int steps, BaseVector & hu, BaseVector & hu_init,
                       LocalHeap & lh);

};


#endif // CONSERVATIONLAW_HPP
