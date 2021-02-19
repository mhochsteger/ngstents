#ifndef CONSERVATIONLAW_TP_IMPL
#define CONSERVATIONLAW_TP_IMPL

#include "conservationlaw.hpp"
#include "paralleldepend.hpp"
#include "tentsolver_impl.hpp"

template <typename EQUATION, int DIM, int COMP, int ECOMP>
void T_ConservationLaw<EQUATION, DIM, COMP, ECOMP>::
CalcFluxTent (const Tent & tent, FlatMatrixFixWidth<COMP> u, FlatMatrixFixWidth<COMP> u0,
	      FlatMatrixFixWidth<COMP> flux, double tstar, LocalHeap & lh)
{
  static Timer tflux ("CalcFluxTent", 2);
  ThreadRegionTimer reg(tflux, TaskManager::GetThreadId());

  // note: tstar is not used
  auto fedata = tent.fedata;
  if (!fedata) throw Exception("fedata not set");

  // these variables no longer exist
  *(tent.time) = tent.timebot + tstar*(tent.ttop-tent.tbot);

  flux = 0.0;
  {
  for (int i : Range(tent.els))
    {
      HeapReset hr(lh);
      const DGFiniteElement<DIM> & fel =
	static_cast<const DGFiniteElement<DIM>&> (*fedata->fei[i]);

      auto & simd_ir = *fedata->iri[i];
      auto & simd_mir = *fedata->miri[i];

      IntRange dn = fedata->ranges[i];

      FlatMatrix<SIMD<double>> u_iptsa(COMP, simd_ir.Size(),lh);
      FlatMatrix<SIMD<double>> flux_iptsa(DIM*COMP, simd_ir.Size(),lh);
      FlatMatrix<SIMD<double>> flux_iptsa2(DIM*COMP, simd_ir.Size(),lh);

      fel.Evaluate (simd_ir, u.Rows(dn), u_iptsa);

      Cast().Flux(simd_mir, u_iptsa, flux_iptsa);

      FlatVector<SIMD<double>> di = fedata->adelta[i];
      for (auto k : Range(simd_ir.Size()))
        flux_iptsa.Col(k) *= simd_mir[k].GetWeight() * di(k);
      {
        for (size_t i = 0; i < COMP; i++)
          for (size_t j = 0; j < DIM; j++)
            flux_iptsa2.Row(i*DIM+j) = flux_iptsa.Row(j*COMP+i);

        fel.AddGradTrans (simd_mir, flux_iptsa2, flux.Rows(dn));
      }
    }
  }

  {
  for(int i : Range(tent.internal_facets))
    {
      HeapReset hr(lh);
      size_t elnr1 = fedata->felpos[i][0];
      size_t elnr2 = fedata->felpos[i][1];
      if(elnr2 != size_t(-1))
        {
          // inner facet
          const DGFiniteElement<DIM> & fel1 =
            static_cast<const DGFiniteElement<DIM>&> (*fedata->fei[elnr1]);
          const DGFiniteElement<DIM> & fel2 =
            static_cast<const DGFiniteElement<DIM>&> (*fedata->fei[elnr2]);

          IntRange dn1 = fedata->ranges[elnr1];
          IntRange dn2 = fedata->ranges[elnr2];

          auto & simd_ir_facet_vol1 = *fedata->firi[i][0];
          auto & simd_ir_facet_vol2 = *fedata->firi[i][1];

          int simd_nipt = simd_ir_facet_vol1.Size(); // IR's have the same size
          FlatMatrix<SIMD<double>> u1(COMP, simd_nipt, lh),
                                   u2(COMP, simd_nipt, lh);

          fel1.Evaluate(simd_ir_facet_vol1, u.Rows(dn1), u1);
          fel2.Evaluate(simd_ir_facet_vol2, u.Rows(dn2), u2);

          auto & simd_mir1 = *fedata->mfiri1[i];

          FlatMatrix<SIMD<double>> fn(COMP, simd_nipt, lh);
	  Cast().NumFlux (simd_mir1, u1, u2, fedata->anormals[i], fn);

          FlatVector<SIMD<double>> di = fedata->adelta_facet[i];
          for (size_t j : Range(simd_nipt))
            fn.Col(j) *= -1.0 * di(j) * simd_mir1[j].GetWeight();

          fel1.AddTrans(simd_ir_facet_vol1, fn, flux.Rows(dn1));
          fn *= -1.0;
          fel2.AddTrans(simd_ir_facet_vol2, fn, flux.Rows(dn2));
        }
      else
        {
          // boundary facet
          const DGFiniteElement<DIM> & fel1 =
            static_cast<const DGFiniteElement<DIM>&> (*fedata->fei[elnr1]);

          IntRange dn1 = fedata->ranges[elnr1];

          auto & simd_ir_facet_vol1 = *fedata->firi[i][0];

          int simd_nipt = simd_ir_facet_vol1.Size(); // IR's have the same size
          FlatMatrix<SIMD<double>> u1(COMP, simd_nipt, lh),
                                   u2(COMP, simd_nipt, lh);

          fel1.Evaluate(simd_ir_facet_vol1,u.Rows(dn1),u1);
          auto & simd_mir = *fedata->mfiri1[i];

          // check for boundary condition number
          int bc = bcnr[tent.internal_facets[i]];
          if (bc == 0) // outflow, use same values as on the inside
            {
              u2 = u1;
            }
          else if (bc == 1) // wall
            {
              Cast().u_reflect(simd_mir, u1, fedata->anormals[i], u2);
            }
          else if (bc == 2) // inflow, use initial data
            {
              fel1.Evaluate(simd_ir_facet_vol1, u0.Rows(dn1), u2);
            }
          else if (bc == 3) // transparent (wave)
            {
              Cast().u_transparent(simd_mir, u1, fedata->anormals[i], u2);
            }
          else
	    {
	      if(cf_bnd[bc])
                {
                  cf_bnd[bc]->Evaluate(simd_mir,u2);
                }
              else
		throw Exception(string("no implementation for your") +
				string(" chosen boundary condition number ") +
				ToString(bc+1));
	    }

          FlatMatrix<SIMD<double>> fn(COMP, simd_nipt, lh);
	  Cast().NumFlux (simd_mir, u1, u2, fedata->anormals[i], fn);

          FlatVector<SIMD<double>> di = fedata->adelta_facet[i];
          for (size_t j : Range(simd_nipt))
            {
              auto fac = -1.0 * di(j) * simd_mir[j].GetWeight();
              fn.Col(j) *= fac;
            }

          fel1.AddTrans(simd_ir_facet_vol1, fn, flux.Rows(dn1));
        }
    }
  }

  for (int i : Range (tent.els))
    SolveM (tent, i, flux.Rows (tent.fedata->ranges[i]), lh);

}

template <typename EQUATION, int DIM, int COMP, int ECOMP>
void T_ConservationLaw<EQUATION, DIM, COMP, ECOMP>::
CalcViscosityTent (const Tent & tent, FlatMatrixFixWidth<COMP> u,
                   FlatMatrixFixWidth<COMP> ubnd, FlatVector<double> nu,
                   FlatMatrixFixWidth<COMP> visc, LocalHeap & lh)
{
  // const Tent & tent = tps->GetTent(tentnr);

  // grad(u)*grad(v) - {du/dn} * [v] - {dv/dn} * [u] + alpha * p^2 / h * [u]*[v]
  double alpha = 4.0;
  alpha *= sqr(fes->GetOrder());

  auto fedata = tent.fedata;
  if (!fedata) throw Exception("fedata not set");

  visc = 0.0;
  for (size_t i : Range(tent.els))
    {
      HeapReset hr(lh);
      const DGFiniteElement<DIM> & fel =
	static_cast<const DGFiniteElement<DIM>&> (*fedata->fei[i]);
      auto & simd_mir = *fedata->miri[i];

      IntRange dn = tent.fedata->ranges[i];

      FlatMatrix<SIMD<double>> gradu(DIM,simd_mir.Size(),lh);
      for(size_t j : Range(COMP))
        {
	  fel.EvaluateGrad(simd_mir,u.Col(j).Range(dn),gradu);
	  for(size_t k : Range(simd_mir.Size()))
	    gradu.Col(k) *= nu(i) * simd_mir[k].GetWeight();

	  fel.AddGradTrans(simd_mir,gradu,visc.Col(j).Range(dn));
        }

      //quick hack for other facets
      auto fnums = ma->GetElFacets (tent.els[i]);
      for(auto j : Range(fnums))
        if(!tent.internal_facets.Contains(fnums[j]))
          {
            int locfacetnr = j;
            auto & trafo = *fedata->trafoi[i];
            auto etfacet = ElementTopology::GetFacetType (trafo.GetElementType(),
                                                          locfacetnr);
            // note: need to add 1 to integration order
            SIMD_IntegrationRule fir (etfacet,2*fel.Order()+1);
            auto vnums = ma->GetElVertices (ElementId(VOL,tent.els[i]));
            Facet2ElementTrafo transform(trafo.GetElementType(), vnums);
            auto & simd_ir_facet_vol = transform(locfacetnr,fir,lh);
            auto & simd_mfir = trafo(simd_ir_facet_vol,lh);
            simd_mfir.ComputeNormalsAndMeasure(trafo.GetElementType(),locfacetnr);

            int simd_nipt = simd_ir_facet_vol.Size();
            FlatMatrix<SIMD<double>>
              u1(COMP, simd_nipt, lh), u2(COMP, simd_nipt,lh),
              jumpu(COMP, simd_nipt, lh), gradu(DIM, simd_nipt, lh),
              dudn(COMP, simd_nipt, lh), temp(DIM, simd_nipt, lh);

            fel.Evaluate(simd_ir_facet_vol,u.Rows(dn),u1);
            fel.Evaluate(simd_ir_facet_vol,ubnd.Rows(dn),u2);
            jumpu = u1-u2;

            FlatVector<SIMD<double>> fac(simd_nipt,lh);
            for(size_t k : Range(simd_nipt))
              {
                fac(k) = nu(i)*simd_mfir[k].GetWeight();
                jumpu.Col(k) *= fac(k);
              }

            auto normal = simd_mfir.GetNormals();
            for(size_t j : Range(COMP))
              {
                fel.EvaluateGrad(simd_mfir,u.Col(j).Range(dn),gradu);
                for(size_t k : Range(simd_nipt))
                  {
                    dudn(j,k) = -fac(k)*InnerProduct(gradu.Col(k),normal.Row(k));
                    temp.Col(k) = -jumpu(j,k) * normal.Row(k);
                  }
                fel.AddGradTrans(simd_mfir,temp,visc.Col(j).Range(dn));
              }
            auto h = fabs(simd_mfir[0].GetJacobiDet())/simd_mfir[0].GetMeasure();
            jumpu *= alpha / h;
            dudn += jumpu;
            fel.AddTrans(simd_ir_facet_vol,dudn,visc.Rows(dn));
          }
    }

  for(size_t i : Range(tent.internal_facets))
    {
      size_t elnr1 = fedata->felpos[i][0];
      size_t elnr2 = fedata->felpos[i][1];
      if(elnr2 != size_t(-1))
        {
          // inner facet
          HeapReset hr(lh);
          const DGFiniteElement<DIM> & fel1 =
            static_cast<const DGFiniteElement<DIM>&> (*fedata->fei[elnr1]);
          const DGFiniteElement<DIM> & fel2 =
            static_cast<const DGFiniteElement<DIM>&> (*fedata->fei[elnr2]);

          IntRange dn1 = tent.fedata->ranges[elnr1];
          IntRange dn2 = tent.fedata->ranges[elnr2];

          auto & simd_ir_facet_vol1 = *fedata->firi[i][0];
          auto & simd_ir_facet_vol2 = *fedata->firi[i][1];
          auto & simd_mir = *fedata->mfiri1[i];
          auto & simd_mir2 = *fedata->mfiri2[i];

          int simd_nipt = simd_ir_facet_vol1.Size(); // IR's have the same size
          FlatMatrix<SIMD<double>>
            u1(COMP, simd_nipt, lh), u2(COMP, simd_nipt, lh),
            gradu1(DIM, simd_nipt, lh), gradu2(DIM, simd_nipt, lh),
            jumpu(COMP, simd_nipt, lh), dudn(COMP, simd_nipt, lh),
            temp(DIM, simd_nipt, lh);

          fel1.Evaluate(simd_ir_facet_vol1,u.Rows(dn1),u1);
          fel2.Evaluate(simd_ir_facet_vol2,u.Rows(dn2),u2);
          jumpu = nu(elnr1)*u1-nu(elnr2)*u2;

          FlatVector<SIMD<double>> fac(simd_nipt,lh);
          for(size_t k : Range(simd_nipt))
            {
              fac(k) = simd_mir[k].GetWeight();
              jumpu.Col(k) *= fac(k);
            }

          auto normal = fedata->anormals[i];
          // *testout << normal << endl;
          for(size_t j : Range(COMP))
            {
              fel1.EvaluateGrad(simd_mir,u.Col(j).Range(dn1),gradu1);
	      fel2.EvaluateGrad(simd_mir2,u.Col(j).Range(dn2),gradu2);
	      for(size_t k : Range(simd_nipt))
		{
                  temp.Col(k) = -0.5 * jumpu(j,k) * normal.Col(k);
                  dudn(j,k) = InnerProduct(nu(elnr1)*gradu1.Col(k) +
                                           nu(elnr2)*gradu2.Col(k),normal.Col(k));
		  dudn(j,k) *= -0.5 * fac(k);
		}
	      fel1.AddGradTrans(simd_mir,temp,visc.Col(j).Range(dn1));
	      temp *= -1.0;
              fel2.AddGradTrans(simd_mir2,temp,visc.Col(j).Range(dn2));
            }
          auto h = fabs(simd_mir[0].GetJacobiDet())/simd_mir[0].GetMeasure();
          jumpu *= alpha / h;
          dudn += jumpu;
          fel1.AddTrans(simd_ir_facet_vol1,dudn,visc.Rows(dn1));
	  dudn *= -1.0;
	  fel2.AddTrans(simd_ir_facet_vol2,dudn,visc.Rows(dn2));
        }
    }

  for (int i : Range (tent.els))
    {
      SolveM (tent, i, fedata->adelta[i], visc.Rows (tent.fedata->ranges[i]), lh);
    }
}

template <typename EQUATION, int DIM, int COMP, int ECOMP>
void T_ConservationLaw<EQUATION, DIM, COMP, ECOMP>::
CalcEntropyResidualTent (const Tent & tent, FlatMatrixFixWidth<COMP> u,
                         FlatMatrixFixWidth<COMP> ut,
                         FlatMatrixFixWidth<ECOMP> res,
                         FlatMatrixFixWidth<COMP> u0, double tstar,
                         LocalHeap & lh)
{
  HeapReset hr(lh);

  // const Tent & tent = tps->GetTent(tentnr);
  auto fedata = tent.fedata;
  if (!fedata) throw Exception("fedata not set");

  res = 0.0;
  for (int i : Range(tent.els))
    {
      HeapReset hr(lh);
      const DGFiniteElement<DIM> & fel =
	static_cast<const DGFiniteElement<DIM>&> (*fedata->fei[i]);
      const SIMD_IntegrationRule & simd_ir = *fedata->iri[i];

      IntRange dn = tent.fedata->ranges[i];

      int ndof = fel.GetNDof();

      FlatVector<> uiel_coeff(ndof,lh);
      FlatVector<> cons_coeff(ndof,lh);

      FlatMatrix<SIMD<double>> ui(COMP,simd_ir.Size(),lh),
                               uti(COMP,simd_ir.Size(),lh);
      fel.Evaluate(simd_ir,u.Rows(dn),ui);
      fel.Evaluate(simd_ir,ut.Rows(dn),uti);

      FlatMatrix<AutoDiff<1,SIMD<double>>> adu(COMP,simd_ir.Size(),lh);
      for(size_t k : Range(COMP))
        for(size_t l : Range(simd_ir.Size()))
          {
            adu(k,l).Value() = ui(k,l);
            adu(k,l).DValue(0) = uti(k,l);
          }

      auto & simd_mir = *fedata->miri[i];

      auto gradbot = fedata->agradphi_bot[i];
      auto gradtop = fedata->agradphi_top[i];
      FlatMatrix<AutoDiff<1,SIMD<double>>> gradphi_mat(DIM, simd_mir.Size(), lh);
      for(size_t k : Range(DIM))
        for(size_t l : Range(simd_mir.Size()))
          {
            gradphi_mat(k,l).Value() = (1-tstar)*gradbot(k,l) + tstar*gradtop(k,l);
            gradphi_mat(k,l).DValue(0) = gradtop(k,l) - gradbot(k,l);
          }

      Cast().InverseMap(simd_mir, gradphi_mat, adu);
      FlatMatrix<SIMD<double>> Ei(ECOMP,simd_ir.Size(),lh),
                               Fi(DIM*ECOMP,simd_ir.Size(),lh);
      Cast().CalcEntropy(adu, gradphi_mat, Ei, Fi);

      FlatVector<SIMD<double>> di = fedata->adelta[i];
      for(size_t k : Range(simd_ir.Size()))
        {
          Ei(0,k) *= simd_mir[k].GetWeight();
          auto fac = -1.0 * simd_mir[k].GetWeight() * di(k);
          for(size_t l : Range(DIM))
            Fi(l,k) *= fac;
          if(ECOMP>1)
            throw Exception(
                "not yet implemented for more than one entropy function");
        }

      fel.AddTrans(simd_ir,Ei,res.Rows(dn));
      fel.AddGradTrans (simd_mir, Fi, res.Col(0).Range(dn));
    }

  FlatMatrixFixWidth<COMP> temp(u.Height(),lh);
  Cyl2Tent(tent, tstar, u, temp, lh);

  for(int i : Range(tent.internal_facets))
    {
      HeapReset hr(lh);
      size_t elnr1 = fedata->felpos[i][0];
      size_t elnr2 = fedata->felpos[i][1];

      if(elnr2 != size_t(-1))
        {
          // inner facet
          const DGFiniteElement<DIM> & fel1 =
            static_cast<const DGFiniteElement<DIM>&> (*fedata->fei[elnr1]);
          const DGFiniteElement<DIM> & fel2 =
            static_cast<const DGFiniteElement<DIM>&> (*fedata->fei[elnr2]);

          IntRange dn1 = tent.fedata->ranges[elnr1];
          IntRange dn2 = tent.fedata->ranges[elnr2];

          auto & simd_ir_facet_vol1 = *fedata->firi[i][0];
          auto & simd_ir_facet_vol2 = *fedata->firi[i][1];

          int simd_nipt = simd_ir_facet_vol1.Size(); // IR's have the same size
          FlatMatrix<SIMD<double>> u1(COMP, simd_nipt, lh),
                                   u2(COMP, simd_nipt, lh);

          fel1.Evaluate(simd_ir_facet_vol1,temp.Rows(dn1),u1);
          fel2.Evaluate(simd_ir_facet_vol2,temp.Rows(dn2),u2);

          FlatMatrix<SIMD<double>> Fn(ECOMP, simd_nipt, lh);
          Cast().EntropyFlux(u1,u2,fedata->anormals[i],Fn);

          auto & simd_mir = *fedata->mfiri1[i];
          FlatVector<SIMD<double>> di = fedata->adelta_facet[i];
          for (size_t j : Range(simd_nipt))
            {
              auto fac = di(j) * simd_mir[j].GetWeight();
              Fn.Col(j) *= fac;
            }

          fel1.AddTrans(simd_ir_facet_vol1,Fn,res.Rows(dn1));
          Fn *= -1.0;
          fel2.AddTrans(simd_ir_facet_vol2,Fn,res.Rows(dn2));
        }
      else
        {
          // boundary facet
          const DGFiniteElement<DIM> & fel1 =
            static_cast<const DGFiniteElement<DIM>&> (*fedata->fei[elnr1]);

          IntRange dn1 = tent.fedata->ranges[elnr1];

          auto & simd_ir_facet_vol1 = *fedata->firi[i][0];

          int simd_nipt = simd_ir_facet_vol1.Size();
          FlatMatrix<SIMD<double>> u1(COMP, simd_nipt, lh),
                                   u2(COMP, simd_nipt, lh);

          fel1.Evaluate(simd_ir_facet_vol1,temp.Rows(dn1),u1);

	  auto & simd_mir1 = *fedata->mfiri1[i];
	  // set u2 dofs based on boundary condition number
          int bc = bcnr[tent.internal_facets[i]];
          if (bc == 0) // outflow, use same values as on the inside
            {
              u2 = u1;
            }
          else if (bc == 1) // wall
            {
              Cast().u_reflect(simd_mir1, u1, fedata->anormals[i], u2);
            }
          else if (bc == 2) // inflow, use initial data
            {
              fel1.Evaluate(simd_ir_facet_vol1, u0.Rows(dn1), u2);
            }
          else if (bc == 3) // transparent (wave)
            {
              Cast().u_transparent(simd_mir1, u1, fedata->anormals[i], u2);
            }
          else
            throw Exception(string(
               "no implementation for your chosen boundary condition number ")
                + ToString(bc+1));

          FlatMatrix<SIMD<double>> Fn(ECOMP, simd_nipt, lh);
          Cast().EntropyFlux(u1,u2,fedata->anormals[i],Fn);

	  FlatVector<SIMD<double>> di = fedata->adelta_facet[i];
          for (size_t j : Range(simd_nipt))
            {
              auto fac = di(j) * simd_mir1[j].GetWeight();
              Fn.Col(j) *= fac;
            }
          if(bc!=1)
            fel1.AddTrans(simd_ir_facet_vol1,Fn,res.Rows(dn1));
        }
    }
  for (int i : Range (tent.els))
    SolveM (tent, i, res.Rows (tent.fedata->ranges[i]), lh);
}

template <typename EQUATION, int DIM, int COMP, int ECOMP>
double T_ConservationLaw<EQUATION, DIM, COMP, ECOMP>::
CalcViscosityCoefficientTent (const Tent & tent, FlatMatrixFixWidth<COMP> u,
                              FlatMatrixFixWidth<ECOMP> res,
			      double tstar, LocalHeap & lh)
{
  // const Tent & tent = tps->GetTent(tentnr);
  auto fedata = tent.fedata;
  if (!fedata) throw Exception("fedata not set");

  double nu_tent = 0.0;

  for (int i : Range(tent.els))
    {
      HeapReset hr(lh);
      ElementId ei (VOL, tent.els[i]);
      const DGFiniteElement<DIM> & fel =
	static_cast<const DGFiniteElement<DIM>&> (*fedata->fei[i]);
      const SIMD_IntegrationRule & simd_ir = *fedata->iri[i];

      IntRange dn = tent.fedata->ranges[i];

      FlatMatrix<SIMD<double>> resi(ECOMP,simd_ir.Size(),lh);
      FlatMatrix<SIMD<double>> ui(COMP,simd_ir.Size(),lh);

      auto & simd_mir = *fedata->miri[i];
      double hi = pow(simd_mir[0].GetMeasure()[0]/DIM,1.0/DIM);
      if( fel.Order() > 0 ) hi /= fel.Order();

      fel.Evaluate(simd_ir,u.Rows(dn),ui);
      fel.Evaluate(simd_ir,res.Rows(dn),resi);

      FlatVector<SIMD<double>> di = fedata->adelta[i];
      for(size_t k : Range(simd_ir.Size()))
        resi.Col(k) /= di(k);

      // clear overhead
      FlatMatrix<double> temp(ECOMP,simd_ir.Size()*SIMD<double>::Size(),
                              &resi(0,0)[0]);
      temp.Cols(simd_ir.GetNIP(),simd_ir.Size()*SIMD<double>::Size()) = 0.0;
      FlatMatrix<double> tempu(COMP,simd_ir.Size()*SIMD<double>::Size(),
                               &ui(0,0)[0]);
      tempu.Cols(simd_ir.GetNIP(),simd_ir.Size()*SIMD<double>::Size()) = 0.0;

      FlatMatrix<SIMD<double>> gradphi_mat(DIM, simd_mir.Size(), lh);
      gradphi_mat = (1-tstar)*fedata->agradphi_bot[i] +
                    tstar*fedata->agradphi_top[i];

      Cast().InverseMap(simd_mir, gradphi_mat, ui);
      Cast().CalcViscCoeffEl(simd_mir, ui, resi, hi, nu(ei.Nr()));

      if(nu(ei.Nr()) > nu_tent)
	nu_tent = nu(ei.Nr());
    }
  return nu_tent;
}

////////////////////////////////////////////////////////////////
// implementations of maps 
////////////////////////////////////////////////////////////////

template <typename EQUATION, int DIM, int COMP, int ECOMP>
void T_ConservationLaw<EQUATION, DIM, COMP, ECOMP>::
Cyl2Tent (const Tent & tent, double tstar,
	  FlatMatrixFixWidth<COMP> uhat, FlatMatrixFixWidth<COMP> u,
	  LocalHeap & lh)
{
  static Timer tcyl2tent ("Cyl2Tent", 2);
  ThreadRegionTimer reg(tcyl2tent, TaskManager::GetThreadId());

  auto fedata = tent.fedata;
  if (!fedata) throw Exception("fedata not set");

  for (size_t i : Range(tent.els))
    {
      HeapReset hr(lh);
      const DGFiniteElement<DIM> & fel =
        static_cast<const DGFiniteElement<DIM>&> (*fedata->fei[i]);

      auto & simd_mir = *fedata->miri[i];
      IntRange dn = fedata->ranges[i];

      FlatMatrix<SIMD<double>> u_ipts(COMP, simd_mir.Size(),lh);
      for(size_t k = 0; k < COMP; k++)
        fel.Evaluate (simd_mir.IR(), uhat.Col(k).Range(dn), u_ipts.Row(k));

      FlatMatrix<SIMD<double>> gradphi_mat(DIM, simd_mir.Size(), lh);
      gradphi_mat = (1-tstar)*fedata->agradphi_bot[i] +
                    tstar*fedata->agradphi_top[i];

      Cast().InverseMap(simd_mir, gradphi_mat, u_ipts);

      for(size_t k = 0; k < simd_mir.Size(); k++)
        u_ipts.Col(k) *= simd_mir[k].GetWeight();

      u.Rows(dn) = 0.0;
      for(size_t k = 0; k < COMP; k++)
        fel.AddTrans (simd_mir.IR(), u_ipts.Row(k), u.Col(k).Range(dn));

      SolveM (tent, i, u.Rows (dn), lh);
    }
}

template <typename EQUATION, int DIM, int COMP, int ECOMP>
void T_ConservationLaw<EQUATION, DIM, COMP, ECOMP>::
ApplyM1 (const Tent & tent, double tstar, FlatMatrixFixWidth<COMP> u,
         FlatMatrixFixWidth<COMP> res, LocalHeap & lh)
{
  static Timer tapplym1 ("ApplyM1", 2);
  ThreadRegionTimer reg(tapplym1, TaskManager::GetThreadId());

  auto fedata = tent.fedata;
  if (!fedata) throw Exception("fedata not set");

  res = 0.0;
  for (int i : Range(tent.els))
    {
      HeapReset hr(lh);
      const DGFiniteElement<DIM> & fel =
	static_cast<const DGFiniteElement<DIM>&> (*fedata->fei[i]);

      const SIMD_IntegrationRule & simd_ir = *fedata->iri[i];
      IntRange dn = fedata->ranges[i];

      FlatMatrix<SIMD<double>> u_ipts(COMP, simd_ir.Size(), lh),
                               temp(COMP, simd_ir.Size(), lh);
      FlatMatrix<SIMD<double>> flux(COMP*DIM, simd_ir.Size(), lh);
      FlatMatrix<SIMD<double>> graddelta_mat(DIM, simd_ir.Size(), lh);
      graddelta_mat = fedata->agradphi_top[i] - fedata->agradphi_bot[i];

      fel.Evaluate (simd_ir, u.Rows(dn), u_ipts);

      auto & simd_mir = *fedata->miri[i];

      Cast().Flux(simd_mir,u_ipts,flux);
      for(size_t j : Range(simd_ir.Size()))
        for(size_t l : Range(COMP))
          {
            SIMD<double> hsum(0.0);
            for(size_t k : Range(DIM))
              {
                auto graddelta = graddelta_mat(k,j) * simd_mir[j].GetWeight();
                hsum += graddelta * flux(COMP*k+l,j);
              }
            temp(l,j) = hsum;
          }
      fel.AddTrans(simd_ir,temp,res.Rows(dn));

      SolveM (tent, i, res.Rows (dn), lh);
    }
}

template <typename EQUATION, int DIM, int COMP, int ECOMP>
void T_ConservationLaw<EQUATION, DIM, COMP, ECOMP>::
Tent2Cyl (const Tent & tent, double tstar,
	  FlatMatrixFixWidth<COMP> u, FlatMatrixFixWidth<COMP> uhat,
          bool solvemass, LocalHeap & lh)
{
  static Timer ttent2cyl ("Tent2Cyl", 2);
  ThreadRegionTimer reg(ttent2cyl, TaskManager::GetThreadId());

  auto fedata = tent.fedata;
  if (!fedata) throw Exception("fedata not set");

  uhat = 0.0;
  for (int i : Range(tent.els))
    {
      HeapReset hr(lh);
      const DGFiniteElement<DIM> & fel =
	static_cast<const DGFiniteElement<DIM>&> (*fedata->fei[i]);
      const SIMD_IntegrationRule & simd_ir = *fedata->iri[i];

      IntRange dn = fedata->ranges[i];

      FlatMatrix<SIMD<double>> u_ipts(COMP, simd_ir.Size(), lh);
      FlatMatrix<SIMD<double>> flux(COMP*DIM, simd_ir.Size(), lh),
                               res(COMP, simd_ir.Size(), lh);
      FlatMatrix<SIMD<double>> gradphi_mat(DIM, simd_ir.Size(), lh);
      gradphi_mat = (1-tstar)*fedata->agradphi_bot[i] +
                    tstar*fedata->agradphi_top[i];

      fel.Evaluate (simd_ir, u.Rows(dn), u_ipts);

      auto & simd_mir = *fedata->miri[i];
      Cast().Flux(simd_mir,u_ipts,flux);

      for(size_t j : Range(simd_ir.Size()))
        for(size_t l : Range(COMP))
          {
            SIMD<double> hsum(0.0);
            for(size_t k : Range(DIM))
              {
                auto gradphi = gradphi_mat(k,j) * simd_mir[j].GetWeight();
                hsum += gradphi * flux(COMP*k+l,j);
              }
            res(l,j) = u_ipts(l,j) * simd_mir[j].GetWeight() - hsum;
          }

      fel.AddTrans(simd_ir,res,uhat.Rows(dn));
      if(solvemass)
	SolveM (tent, i, uhat.Rows (dn), lh);
    }
}

////////////////////////////////////////////////////////////////
// time stepping methods 
////////////////////////////////////////////////////////////////

template <typename EQUATION, int DIM, int COMP, int ECOMP>
void T_ConservationLaw<EQUATION, DIM, COMP, ECOMP>::
Propagate(LocalHeap & lh)
{
  static Timer tprop ("Propagate", 2); RegionTimer reg(tprop);
  RunParallelDependency
    (tent_dependency, [&] (int i)
     {
       LocalHeap slh = lh.Split();  // split to threads
       tentsolver->PropagateTent(tps->GetTent(i), *u, *uinit, slh);
     });
}

#endif // CONSERVATIONLAW_TP_IMPL
