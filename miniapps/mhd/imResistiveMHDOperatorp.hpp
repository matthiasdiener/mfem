#include "mfem.hpp"
#include <iostream>
#include <fstream>

using namespace std;
using namespace mfem;

/** After spatial discretization, the resistive MHD model can be written as a
 *  system of ODEs:
 *     dPsi/dt = M^{-1}*F1,
 *     dw  /dt = M^{-1}*F2,
 *  coupled with two linear systems
 *     j   = -M^{-1}*(K-B)*Psi 
 *     Phi = -K^{-1}*M*w
 *  so far there seems no need to do a BlockNonlinearForm
 *
 *  Class ResistiveMHDOperator represents the right-hand side of the above
 *  system of ODEs. */
class ResistiveMHDOperator : public TimeDependentOperator
{
protected:
   ParFiniteElementSpace &fespace;
   Array<int> ess_tdof_list;

   ParBilinearForm *M, *K, *KB, DSl, DRe; //mass, stiffness, diffusion with SL and Re
   ParBilinearForm *Nv, *Nb;
   ParLinearForm *E0, *Sw; //two source terms
   HypreParMatrix Kmat, Mmat;
   HypreParVector *E0Vec;
   double viscosity, resistivity;
   double jBdy;
   bool useAMG;

   //for implicit stepping
   ReducedSystemOperator *reduced_oper;
   PetscNonlinearSolver* pnewton_solver;
   PetscPreconditionerFactory *J_factory;

   CGSolver M_solver; // Krylov solver for inverting the mass matrix M
   HypreSmoother M_prec;  // Preconditioner for the mass matrix M

   CGSolver K_solver; // Krylov solver for inverting the stiffness matrix K
   HypreSmoother K_prec;  // Preconditioner for the stiffness matrix K

   HypreSolver *K_amg; //BoomerAMG for stiffness matrix
   HyprePCG *K_pcg;

   ParGridFunction j;

   mutable Vector z, zFull; // auxiliary vector 
   mutable ParGridFunction gf;  //auxiliary variable (to store the boundary condition)

public:
   ResistiveMHDOperator(ParFiniteElementSpace &f, Array<int> &ess_bdr, 
                       double visc, double resi); 

   // Compute the right-hand side of the ODE system.
   virtual void Mult(const Vector &vx, Vector &dvx_dt) const;

   //Solve the Backward-Euler equation: k = f(x + dt*k, t), for the unknown k.
   //here vector are block vectors
   virtual void ImplicitSolve(const double dt, const Vector &x, Vector &k);

   //link gf with psi
   void BindingGF(Vector &vx)
   {int sc = height/3; gf.MakeTRef(&fespace, vx, sc);}

   //set rhs E0 
   void SetRHSEfield(FunctionCoefficient Efield);
   void SetInitialJ(FunctionCoefficient initJ);
   void SetJBdy(double jBdy_) 
   {jBdy = jBdy_;}

   void UpdatePhi(Vector &vx);
   void assembleNv(ParGridFunction *gf);
   void assembleNb(ParGridFunction *gf);

   void DestroyHypre();
   virtual ~ResistiveMHDOperator();
};

// reduced system (it will not own anything)
class ReducedSystemOperator : public Operator
{
private:
   ParFiniteElementSpace &fespace;
   ParBilinearForm *M, *K, *DRe, *DSl;
   HypreParMatrix &Mmat, &Kmat;
   HypreParMatrix Mdt;
   HypreParVector *E0Vec;
   ParGridFunction *j0;

   CGSolver *M_solver;

   double dt;
   const Vector *phi, *psi, *w;
   const Array<int> &ess_tdof_list;

   mutable ParGridFunction phiGf, psiGf;
   mutable ParBilinearForm *Nv, *Nb;
   mutable HypreParMatrix *Jacobian;
   mutable HypreParMatrix Mtmp;
   mutable Vector z, zFull, Z, J;

public:
   ReducedSystemOperator(ParFiniteElementSpace &f, 
                         ParBilinearForm *M_, ParBilinearForm *K_, ParLinearForm *E0_,
                         const Array<int> &ess_tdof_list);

   /// Set current values - needed to compute action and Jacobian.
   void SetParameters(double dt_, const Vector *phi_, const Vector *psi_, const Vector *w_)
   {dt=dt_; phi=phi_; psi=psi_; w=w_;}

   void SetCurrent(ParGridFunction *gf)
   { j0=gf;}

   /// Define F(k) 
   virtual void Mult(const Vector &k, Vector &y) const;

   /// Define J 
   virtual Operator &GetGradient(const Vector &k) const;

   virtual ~ReducedSystemOperator();

};

// Auxiliary class to provide preconditioners for matrix-free methods 
class PreconditionerFactory : public PetscPreconditionerFactory
{
private:
   const ReducedSystemOperator& op;

public:
   PreconditionerFactory(const ReducedSystemOperator& op_,
                         const string& name_): PetscPreconditionerFactory(name_), op(op_) {};
   virtual mfem::Solver* NewPreconditioner(const mfem::OperatorHandle&);
   virtual ~PreconditionerFactory() {};
};

ResistiveMHDOperator::ResistiveMHDOperator(ParFiniteElementSpace &f, 
                                         Array<int> &ess_bdr, double visc, double resi)
   : TimeDependentOperator(3*f.TrueVSize(), 0.0), fespace(f),
     M(NULL), K(NULL), KB(NULL), DSl(&fespace), DRe(&fespace),
     Nv(NULL), Nb(NULL), E0(NULL), Sw(NULL), E0Vec(NULL),
     viscosity(visc),  resistivity(resi), useAMG(false), 
     M_solver(f.GetComm()), K_solver(f.GetComm()), 
     K_amg(NULL), K_pcg(NULL), z(height/3), zFull(f.GetVSize()), j(&fespace)
{
   fespace.GetEssentialTrueDofs(ess_bdr, ess_tdof_list);

   //mass matrix
   M = new ParBilinearForm(&fespace);
   M->AddDomainIntegrator(new MassIntegrator);
   M->Assemble();
   M->FormSystemMatrix(ess_tdof_list, Mmat);

   M_solver.iterative_mode = true;
   M_solver.SetRelTol(1e-12);
   M_solver.SetAbsTol(0.0);
   M_solver.SetMaxIter(2000);
   M_solver.SetPrintLevel(0);
   M_prec.SetType(HypreSmoother::Jacobi);
   M_solver.SetPreconditioner(M_prec);
   M_solver.SetOperator(Mmat);

   //stiffness matrix
   K = new ParBilinearForm(&fespace);
   K->AddDomainIntegrator(new DiffusionIntegrator);
   K->Assemble();
   K->FormSystemMatrix(ess_tdof_list, Kmat);

   useAMG=false;
   if (useAMG)
   {
      K_amg = new HypreBoomerAMG(Kmat);
      K_pcg = new HyprePCG(Kmat);
      K_pcg->iterative_mode = false;
      K_pcg->SetTol(1e-7);
      K_pcg->SetMaxIter(200);
      K_pcg->SetPrintLevel(0);
      K_pcg->SetPreconditioner(*K_amg);
   }
   else
   {
      K_solver.iterative_mode = true;
      K_solver.SetRelTol(1e-7);
      K_solver.SetAbsTol(0.0);
      K_solver.SetMaxIter(2000);
      K_solver.SetPrintLevel(3);
      //K_prec.SetType(HypreSmoother::GS);
      K_prec.SetType(HypreSmoother::Chebyshev); //this is faster
      K_solver.SetPreconditioner(K_prec);
      K_solver.SetOperator(Kmat);
   }

   KB = new ParBilinearForm(&fespace);
   KB->AddDomainIntegrator(new DiffusionIntegrator);      //  K matrix
   KB->AddBdrFaceIntegrator(new BoundaryGradIntegrator);  // -B matrix
   KB->Assemble();

   ConstantCoefficient visc_coeff(viscosity);
   DRe.AddDomainIntegrator(new DiffusionIntegrator(visc_coeff));    
   DRe.Assemble();

   ConstantCoefficient resi_coeff(resistivity);
   DSl.AddDomainIntegrator(new DiffusionIntegrator(resi_coeff));    
   DSl.Assemble();


   //TODO define reducedSystem
}

void ResistiveMHDOperator::SetRHSEfield(FunctionCoefficient Efield) 
{
   delete E0;
   E0 = new ParLinearForm(&fespace);
   E0->AddDomainIntegrator(new DomainLFIntegrator(Efield));
   E0->Assemble();
   E0Vec=E0->ParallelAssemble();
}

void ResistiveMHDOperator::SetInitialJ(FunctionCoefficient initJ) 
{
    j.ProjectCoefficient(initJ);
    j.SetTrueVector();

    //TODO add setCurrent!
}

void ResistiveMHDOperator::Mult(const Vector &vx, Vector &dvx_dt) const
{
   // Create views to the sub-vectors and time derivative
   int sc = height/3;
   dvx_dt=0.0;

   Vector phi(vx.GetData() +   0, sc);
   Vector psi(vx.GetData() +  sc, sc);
   Vector   w(vx.GetData() +2*sc, sc);

   Vector dphi_dt(dvx_dt.GetData() +   0, sc);
   Vector dpsi_dt(dvx_dt.GetData() +  sc, sc);
   Vector   dw_dt(dvx_dt.GetData() +2*sc, sc);

   //for (int i=0; i<ess_tdof_list.Size(); i++)
   //    cout<<j(ess_tdof_list[i])<<" "<<ess_tdof_list[i]<<" "; //set homogeneous Dirichlet condition by hand
   //cout<<endl<<"j size is "<<j.Size()<<endl;

   /*
   //compute the current as an auxilary variable
   KB->Mult(psi, z);
   z.Neg(); // z = -z
   //z.SetSubVector(ess_tdof_list,jBdy);
   //Vector J(sc);
   //M_solver.Mult(z, J);

   HypreParMatrix tmp;
   Vector Y, Z;
   M->FormLinearSystem(ess_tdof_list, j, z, tmp, Y, Z); 
   M_solver.Mult(Z, Y);
   M->RecoverFEMSolution(Y, z, j);
   */


   /*
   cout << "Size of matrix in KB: " <<  KB->Size()<< endl;
   cout << "Size of matrix in Nv: " <<  Nv->Size()<< endl;
   cout << "Size of matrix in DSl: " <<  DSl.Size()<< endl;
   */

   //compute the current as an auxilary variable
   gf.SetFromTrueVector();  //recover psi

   Vector J, Z;
   HypreParMatrix A;
   KB->Mult(gf, zFull);
   zFull.Neg(); // z = -z
   M->FormLinearSystem(ess_tdof_list, j, zFull, A, J, Z); //apply Dirichelt boundary 
   M_solver.Mult(Z, J);

   //evolve the dofs
   z=0.;
   Nv->TrueAddMult(psi, z);
   if (resistivity != 0.0)
   {
      DSl.TrueAddMult(psi, z);
   }
   if (E0Vec!=NULL)
     z += *E0Vec;
   z.Neg(); // z = -z
   z.SetSubVector(ess_tdof_list,0.0);
   M_solver.Mult(z, dpsi_dt);

   z=0.;
   Nv->TrueAddMult(w, z);
   if (viscosity != 0.0)
   {
      DRe.TrueAddMult(w, z);
   }
   z.Neg(); // z = -z
   Nb->TrueAddMult(J, z); 
   z.SetSubVector(ess_tdof_list,0.0);
   M_solver.Mult(z, dw_dt);

}

void HyperelasticOperator::ImplicitSolve(const double dt,
                                         const Vector &vx, Vector &k)
{
   int sc = height/3;
   Vector phi(vx.GetData() +   0, sc);
   Vector psi(vx.GetData() +  sc, sc);
   Vector   w(vx.GetData() +2*sc, sc);

   reduced_oper->SetParameters(dt, &phi, &psi, &w);
   Vector zero; // empty vector is interpreted as zero r.h.s. by NewtonSolver
   pnewton_solver->Mult(zero, k);  //here k is solved as vx^{n+1}
   MFEM_VERIFY(pnewton_solver->GetConverged(),
                  "Newton solver did not converge.");

   //modify k so that it fits into the backward euler framework
   k-=vx;
   k/=dt;
}


void ResistiveMHDOperator::assembleNv(ParGridFunction *gf) 
{
   delete Nv;
   Nv = new ParBilinearForm(&fespace);
   MyCoefficient velocity(gf, 2);   //we update velocity

   Nv->AddDomainIntegrator(new ConvectionIntegrator(velocity));
   Nv->Assemble(); 
}

void ResistiveMHDOperator::assembleNb(ParGridFunction *gf) 
{
   delete Nb;
   Nb = new ParBilinearForm(&fespace);
   MyCoefficient Bfield(gf, 2);   //we update B

   Nb->AddDomainIntegrator(new ConvectionIntegrator(Bfield));
   Nb->Assemble();
}

void ResistiveMHDOperator::UpdatePhi(Vector &vx)
{
   //Phi=-K^{-1}*M*w
   int sc = height/3;
   Vector phi(vx.GetData() +   0, sc);
   Vector   w(vx.GetData() +2*sc, sc);

   Mmat.Mult(w, z);
   z.Neg(); // z = -z
   z.SetSubVector(ess_tdof_list,0.0);

   if (useAMG)
      K_pcg->Mult(z,phi);
   else 
      K_solver.Mult(z, phi);
}

void ResistiveMHDOperator::DestroyHypre()
{
    //hypre needs to be deleted earilier
    delete K_amg;
}


ResistiveMHDOperator::~ResistiveMHDOperator()
{
    //free used memory
    delete M;
    delete K;
    delete E0;
    delete E0Vec;
    delete Sw;
    delete KB;
    delete Nv;
    delete Nb;
    delete K_pcg;
    //delete K_amg;
    //delete M_solver;
    //delete K_solver;
    //delete M_prec;
    //delete K_prec;
}

ReducedSystemOperator::ReducedSystemOperator(ParFiniteElementSpace &f,
   ParBilinearForm *M_, HypreParMatrix &Mmat_,
   ParBilinearForm *K_, HypreParMatrix &Kmat_,
   ParBilinearForm *DRe_, ParBilinearForm *DSl_,
   ParLinearForm *E0_,
   const Array<int> &ess_tdof_list_)
   : Operator(3*f.TrueVSize()), fespace(f), 
     M(M_), Mmat(Mmat_), K(K_),  Kmat(Kmat_), 
     DRe(DRe_), DSl(DSl_), E0(E0_),
     dt(0.0), phi(NULL), psi(NULL), w(NULL), 
     Jacobian(NULL), z(height/3),
     ess_tdof_list(ess_tdof_list_)
{ 
    //looks like I cannot do a deep copy now!
    //maybe aviod it by multplying everything by dt?
    Mdt=
}

void ReducedSystemOperator::Mult(const Vector &k, Vector &y) const
{
   int sc = height/3;

   Vector phiNew(k.GetData() +   0, sc);
   Vector psiNew(k.GetData() +  sc, sc);
   Vector   wNew(k.GetData() +2*sc, sc);

   Vector y1(y.GetData() +   0, sc);
   Vector y2(y.GetData() +  sc, sc);
   Vector y3(y.GetData() +2*sc, sc);

   //------assemble Nv and Nb (operators are assembled locally)------
   delete Nv;
   phiGf.MakeTRef(&fespace, k, 0);
   phiGf.SetFromTrueVector();
   Nv = new ParBilinearForm(&fespace);
   MyCoefficient velocity(&phiGf, 2);   //we update velocity
   Nv->AddDomainIntegrator(new ConvectionIntegrator(velocity));
   Nv->Assemble(); 

   delete Nb;
   psiGf.MakeTRef(&fespace, k, sc);
   psiGf.SetFromTrueVector();
   Nb = new ParBilinearForm(&fespace);
   MyCoefficient Bfield(&psiGf, 2);   //we update B
   Nb->AddDomainIntegrator(new ConvectionIntegrator(Bfield));
   Nb->Assemble();

   //------compute the current as an auxilary variable------
   KB->Mult(psiGF, zFull);
   zFull.Neg(); // z = -z
   M->FormLinearSystem(ess_tdof_list, *j0, zFull, Mtmp, J, Z); //apply Dirichelt boundary 
   M_solver->Mult(Z, J); //XXX is this okay in mult? probably

   //compute y1
   Kmat.Mult(phiNew,y1);
   Mmat.AddMult(wNew,y1);
   y1.SetSubVector(ess_tdof_list, 0.0);

   //compute y2
   z=psiNew-*psi;
   z/=dt;
   Mmat.Mult(z,y2);
   Nv->TrueAddMult(psiNew,y2);
   if (DSl!=NULL)
       DSl->TrueAddMult(psiNew,y2);
   if (E0Vec!=NULL)
       z += *E0Vec;
   y2.SetSubVector(ess_tdof_list, 0.0);

   //compute y3
   z=wNew-*w;
   z/=dt;
   Mmat.Mult(z,y3);
   Nv->TrueAddMult(wNew,y3);
   if (DRe!=NULL)
       DRe->TrueAddMult(wNew,y3);
   Nb->TrueAddMult(J, y3); 
   y3.SetSubVector(ess_tdof_list, 0.0);
}

