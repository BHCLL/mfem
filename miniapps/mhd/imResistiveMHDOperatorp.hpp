#include "mfem.hpp"
#include "PetscPreconditioner.hpp"
#include <iostream>
#include <fstream>

using namespace std;
using namespace mfem;

// reduced system 
class ReducedSystemOperator : public Operator
{
private:
   ParFiniteElementSpace &fespace;
   ParBilinearForm *M, *K, *KB, *DRe, *DSl; 
   HypreParMatrix &Mmat, &Kmat, *DRematpr, *DSlmatpr;
   //own by this:
   HypreParMatrix *Mdtpr, *ARe, *ASl;
   mutable HypreParMatrix *ScFull, *AReFull, *NbFull, *PwMat, Mmatlp, *NbMat;
   bool initialMdt;
   int useFull;
   HypreParVector *E0Vec;
   ParGridFunction *j0;
   Array<int> block_trueOffsets;
   ParBilinearForm *Mlp; 

   CGSolver *M_solver;

   int myid;
   double dt, dtOld;
   const Vector *phi, *psi, *w;
   const Array<int> &ess_tdof_list;
   const Array<int> &ess_bdr;

   mutable ParGridFunction phiGf, psiGf, wGf;
   mutable ParBilinearForm *Nv, *Nb, *Pw;
   mutable ParLinearForm *PB_VPsi, *PB_VOmega, *PB_BJ;
   mutable BlockOperator *Jacobian;
   mutable Vector z, zFull;

public:
   ReducedSystemOperator(ParFiniteElementSpace &f,
                         ParBilinearForm *M_, HypreParMatrix &Mmat_,
                         ParBilinearForm *K_, HypreParMatrix &Kmat_,
                         ParBilinearForm *KB_, ParBilinearForm *DRe_, ParBilinearForm *DSl_,
                         CGSolver *M_solver_, const Array<int> &ess_tdof_list_,
                         const Array<int> &ess_bdr_);

   ReducedSystemOperator(ParFiniteElementSpace &f,
                         ParBilinearForm *M_, HypreParMatrix &Mmat_,
                         ParBilinearForm *K_, HypreParMatrix &Kmat_,
                         ParBilinearForm *KB_, 
                         ParBilinearForm *DRe_, HypreParMatrix *DRemat_,
                         ParBilinearForm *DSl_, HypreParMatrix *DSlmat_,
                         CGSolver *M_solver_, const Array<int> &ess_tdof_list_,
                         const Array<int> &ess_bdr_, int useFull_);

   /// Set current values - needed to compute action and Jacobian.
   void SetParameters(double dt_, const Vector *phi_, const Vector *psi_, const Vector *w_)
   {   
       dtOld=dt; dt=dt_; phi=phi_; psi=psi_; w=w_;
       if (dtOld!=dt && initialMdt)
       {
           if (myid==0) 
              cout <<"------update Mdt------"<<endl;
           double rate=dtOld/dt;
           *Mdtpr*=rate;

           if (useFull == 1)
           {
               delete ARe;
               delete ASl;
               if (DRematpr!=NULL)
                  ARe = ParAdd(Mdtpr, DRematpr);
               else
                  ARe = new HypreParMatrix(*Mdtpr);
                  
               if (DSlmatpr!=NULL)
                  ASl = ParAdd(Mdtpr, DSlmatpr);
               else
                  ASl = new HypreParMatrix(*Mdtpr);
           }
       }
       if(initialMdt==false)
       {
           if (myid==0) 
              cout <<"------initial Mdt-------"<<endl;
           *Mdtpr*=(1./dt); 
           initialMdt=true;

           if (useFull == 1)
           {
              if (DRematpr!=NULL)
                 ARe = ParAdd(Mdtpr, DRematpr);
              else
                 ARe = new HypreParMatrix(*Mdtpr);
                 
              if (DSlmatpr!=NULL)
                 ASl = ParAdd(Mdtpr, DSlmatpr);
              else
                 ASl = new HypreParMatrix(*Mdtpr);
           }
       }
   }

   void setCurrent(ParGridFunction *gf)
   { j0=gf;}

   void setE0(HypreParVector *E0Vec_)
   { E0Vec=E0Vec_;}

   /// Define F(k) 
   virtual void Mult(const Vector &k, Vector &y) const;

   /// Define J 
   virtual Operator &GetGradient(const Vector &k) const;

   virtual ~ReducedSystemOperator();

};

// Auxiliary class to provide preconditioners for matrix-free methods 
class FullPreconditionerFactory : public PetscPreconditionerFactory
{
private:
   const ReducedSystemOperator& op;

public:
   FullPreconditionerFactory(const ReducedSystemOperator& op_,
                         const string& name_): PetscPreconditionerFactory(name_), op(op_) {};
   virtual mfem::Solver* NewPreconditioner(const mfem::OperatorHandle &oh)
   { return new FullBlockSolver(oh);}

   virtual ~FullPreconditionerFactory() {};
};

// Auxiliary class to provide preconditioners for matrix-free methods 
class PreconditionerFactory : public PetscPreconditionerFactory
{
private:
   const ReducedSystemOperator& op;

public:
   PreconditionerFactory(const ReducedSystemOperator& op_,
                         const string& name_): PetscPreconditionerFactory(name_), op(op_) {};
   virtual mfem::Solver* NewPreconditioner(const mfem::OperatorHandle &oh)
   { return new MyBlockSolver(oh);}

   virtual ~PreconditionerFactory() {};
};

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
   HypreParMatrix Kmat, Mmat, DSlmat, DRemat;
   HypreParVector *E0Vec;
   FunctionCoefficient *E0rhs;
   double viscosity, resistivity;
   bool useAMG, use_petsc, use_factory;
   ConstantCoefficient visc_coeff, resi_coeff;

   //for implicit stepping
   ReducedSystemOperator *reduced_oper;
   PetscNonlinearSolver *pnewton_solver;
   PetscPreconditionerFactory *J_factory;

   CGSolver M_solver; // Krylov solver for inverting the mass matrix M
   HypreSmoother *M_prec;  // Preconditioner for the mass matrix M

   CGSolver K_solver; // Krylov solver for inverting the stiffness matrix K
   HypreSmoother *K_prec;  // Preconditioner for the stiffness matrix K

   HypreSolver *K_amg; //BoomerAMG for stiffness matrix
   HyprePCG *K_pcg;

   mutable Vector z, zFull; // auxiliary vector 
   mutable ParGridFunction j, gftmp;  //auxiliary variable (to store the boundary condition)
   ParBilinearForm *DRetmp, *DSltmp;    //hold the matrices for DRe and DSl

public:
   ResistiveMHDOperator(ParFiniteElementSpace &f, Array<int> &ess_bdr, 
                       double visc, double resi, bool use_petsc_, bool use_factory_); 

   // Compute the right-hand side of the ODE system.
   virtual void Mult(const Vector &vx, Vector &dvx_dt) const;

   //Solve the Backward-Euler equation: k = f(x + dt*k, t), for the unknown k.
   //here vector are block vectors
   virtual void ImplicitSolve(const double dt, const Vector &vx, Vector &k);

   //Update problem in AMR case
   void UpdateProblem(Array<int> &ess_bdr);

   //link gftmp with psi; this is an old way
   void BindingGF(Vector &vx)
   {int sc = height/3; gftmp.MakeTRef(&fespace, vx, sc);}

   //update grid functions (grid functions have to be updated immediately)
   void UpdateGridFunction()
   {
      j.Update(); gftmp.Update();

      //DSl and DRe contains ParGridFunctions that need to be updated
      DSl.Update();    
      DSl.Assemble();

      DRe.Update();    
      DRe.Assemble();
   }

   //set rhs E0 
   void SetRHSEfield( double(* f)( const Vector&) );
   void SetInitialJ(FunctionCoefficient initJ);

   void UpdateJ(Vector &k, ParGridFunction *jGf);

   //only for explicit solver
   void UpdatePhi(Vector &vx);
   void assembleNv(ParGridFunction *gf);
   void assembleNb(ParGridFunction *gf);

   void DestroyHypre();
   virtual ~ResistiveMHDOperator();
};


ResistiveMHDOperator::ResistiveMHDOperator(ParFiniteElementSpace &f, 
                                         Array<int> &ess_bdr, double visc, double resi, 
                                         bool use_petsc_ = false, bool use_factory_=false)
   : TimeDependentOperator(3*f.TrueVSize(), 0.0), fespace(f),
     M(NULL), K(NULL), KB(NULL), DSl(&fespace), DRe(&fespace),
     Nv(NULL), Nb(NULL), E0(NULL), Sw(NULL), E0Vec(NULL), E0rhs(NULL),
     viscosity(visc),  resistivity(resi), useAMG(false), use_petsc(use_petsc_), use_factory(use_factory_),
     visc_coeff(visc),  resi_coeff(resi),  
     reduced_oper(NULL), pnewton_solver(NULL), J_factory(NULL),
     M_solver(f.GetComm()), M_prec(NULL), K_solver(f.GetComm()),  K_prec(NULL),
     K_amg(NULL), K_pcg(NULL), z(height/3), zFull(f.GetVSize()), j(&fespace),
     DRetmp(NULL), DSltmp(NULL)
{
   fespace.GetEssentialTrueDofs(ess_bdr, ess_tdof_list);

   //mass matrix
   M = new ParBilinearForm(&fespace);
   M->AddDomainIntegrator(new MassIntegrator);
   M->Assemble();
   M->FormSystemMatrix(ess_tdof_list, Mmat);

   M_solver.iterative_mode = true;
   M_solver.SetRelTol(1e-7);
   M_solver.SetAbsTol(0.0);
   M_solver.SetMaxIter(2000);
   M_solver.SetPrintLevel(0);
   M_prec = new HypreSmoother;
   M_prec->SetType(HypreSmoother::Jacobi);
   M_solver.SetPreconditioner(*M_prec);
   M_solver.SetOperator(Mmat);

   //stiffness matrix
   K = new ParBilinearForm(&fespace);
   K->AddDomainIntegrator(new DiffusionIntegrator);
   K->Assemble();
   K->FormSystemMatrix(ess_tdof_list, Kmat);

   useAMG=true;
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
      delete K_prec;
      K_prec = new HypreSmoother;
      K_prec->SetType(HypreSmoother::Chebyshev);
      K_solver.SetPreconditioner(*K_prec);
      K_solver.SetOperator(Kmat);
   }

   KB = new ParBilinearForm(&fespace);
   KB->AddDomainIntegrator(new DiffusionIntegrator);      //  K matrix
   KB->AddBdrFaceIntegrator(new BoundaryGradIntegrator);  // -B matrix
   KB->Assemble();

   DRe.AddDomainIntegrator(new DiffusionIntegrator(visc_coeff));    
   DRe.Assemble();

   DSl.AddDomainIntegrator(new DiffusionIntegrator(resi_coeff));    
   DSl.Assemble();
  
   if (use_petsc)
   {
      ParBilinearForm *DRepr=NULL, *DSlpr=NULL;
      HypreParMatrix *DRematpr=NULL, *DSlmatpr=NULL;
      if (viscosity != 0.0)
      {   
          //assemble diffusion matrices (cannot delete DRetmp if ParAdd is used later)
          DRetmp = new ParBilinearForm(&fespace);
          DRetmp->AddDomainIntegrator(new DiffusionIntegrator(visc_coeff));    
          DRetmp->Assemble();
          DRetmp->FormSystemMatrix(ess_tdof_list, DRemat);

          DRematpr = &DRemat;
          DRepr = &DRe;
      }

      if (resistivity != 0.0)
      {
          DSltmp = new ParBilinearForm(&fespace);
          DSltmp->AddDomainIntegrator(new DiffusionIntegrator(resi_coeff));    
          DSltmp->Assemble();
          DSltmp->FormSystemMatrix(ess_tdof_list, DSlmat);

          DSlmatpr = &DSlmat;
          DSlpr = &DSl;
      }

      int useFull = 1;
      reduced_oper  = new ReducedSystemOperator(fespace, M, Mmat, K, Kmat,
                         KB, DRepr, DRematpr, DSlpr, DSlmatpr, &M_solver, 
                         ess_tdof_list, ess_bdr, useFull);

      const double rel_tol=1e-4;
      pnewton_solver = new PetscNonlinearSolver(fespace.GetComm(),*reduced_oper);
      if (use_factory)
      {
         SNES snes=SNES(*pnewton_solver);
         KSP ksp; 
		 SNESGetKSP(snes,&ksp);

		 //KSPSetType(ksp,KSPFGMRES);
         //SNESKSPSetUseEW(snes,PETSC_TRUE);
         //SNESKSPSetParametersEW(snes,2,1e-4,0.1,0.9,1.5,1.5,0.1);

         if (useFull==1)
            J_factory = new FullPreconditionerFactory(*reduced_oper, "JFNK Full preconditioner");
         else
            J_factory = new PreconditionerFactory(*reduced_oper, "JFNK preconditioner");
         pnewton_solver->SetPreconditionerFactory(J_factory);
      }
      pnewton_solver->SetPrintLevel(0); // print Newton iterations
      pnewton_solver->SetRelTol(rel_tol);
      pnewton_solver->SetAbsTol(0.0);
      pnewton_solver->SetMaxIter(20);
      pnewton_solver->iterative_mode=true;
   }
}

void ResistiveMHDOperator::UpdateProblem(Array<int> &ess_bdr)
{
   //update ess_tdof_list
   fespace.GetEssentialTrueDofs(ess_bdr, ess_tdof_list);

   //update problem size
   int sc = fespace.GetTrueVSize();
   int scFull = fespace.GetVSize();
   width = height = sc*3;

   //update z holder
   z.SetSize(sc);
   zFull.SetSize(scFull);

   //mass matrix
   M->Update();
   M->Assemble();
   M->FormSystemMatrix(ess_tdof_list, Mmat);

   M_solver.iterative_mode = true;
   M_solver.SetRelTol(1e-7);
   M_solver.SetAbsTol(0.0);
   M_solver.SetMaxIter(2000);
   M_solver.SetPrintLevel(0);
   delete M_prec;
   M_prec = new HypreSmoother;
   M_prec->SetType(HypreSmoother::Jacobi);
   M_solver.SetPreconditioner(*M_prec);
   M_solver.SetOperator(Mmat);

   //stiffness matrix
   K->Update();
   K->Assemble();
   K->FormSystemMatrix(ess_tdof_list, Kmat);

   if (useAMG)
   {
      delete K_amg;
      delete K_pcg;
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
      delete K_prec;
      K_prec = new HypreSmoother;
      K_prec->SetType(HypreSmoother::Chebyshev);
      K_solver.SetPreconditioner(*K_prec);
      K_solver.SetOperator(Kmat);
   }

   KB->Update(); 
   KB->Assemble();


  
   if (use_petsc)
   {
      ParBilinearForm *DRepr=NULL, *DSlpr=NULL;
      HypreParMatrix *DRematpr=NULL, *DSlmatpr=NULL;
      if (viscosity != 0.0)
      {   
          //assemble diffusion matrices (cannot delete DRetmp if ParAdd is used later)
          DRetmp->Update();
          DRetmp->Assemble();
          DRetmp->FormSystemMatrix(ess_tdof_list, DRemat);

          DRematpr = &DRemat;
          DRepr = &DRe;
      }

      if (resistivity != 0.0)
      {
          DSltmp->Update();
          DSltmp->Assemble();
          DSltmp->FormSystemMatrix(ess_tdof_list, DSlmat);

          DSlmatpr = &DSlmat;
          DSlpr = &DSl;
      }

      delete reduced_oper;
      int useFull = 1;
      //if needed, we can replace new with another update function
      reduced_oper  = new ReducedSystemOperator(fespace, M, Mmat, K, Kmat,
                         KB, DRepr, DRematpr, DSlpr, DSlmatpr, &M_solver, 
                         ess_tdof_list, ess_bdr, useFull);

      const double rel_tol=1e-4;
      delete pnewton_solver;
      pnewton_solver = new PetscNonlinearSolver(fespace.GetComm(),*reduced_oper);
      if (use_factory)
      {
         SNES snes=SNES(*pnewton_solver);
         KSP ksp; 
		 SNESGetKSP(snes,&ksp);

         delete J_factory;
         if (useFull==1)
            J_factory = new FullPreconditionerFactory(*reduced_oper, "JFNK Full preconditioner");
         else
            J_factory = new PreconditionerFactory(*reduced_oper, "JFNK preconditioner");
         pnewton_solver->SetPreconditionerFactory(J_factory);
      }
      pnewton_solver->SetPrintLevel(0); // print Newton iterations
      pnewton_solver->SetRelTol(rel_tol);
      pnewton_solver->SetAbsTol(0.0);
      pnewton_solver->SetMaxIter(20);
      pnewton_solver->iterative_mode=true;
   }

   E0->Update();
   E0->Assemble();
   delete E0Vec;
   E0Vec=E0->ParallelAssemble();

   //update E0 
   if (reduced_oper!=NULL)
      reduced_oper->setE0(E0Vec);

   //add current to reduced_oper
   if (reduced_oper!=NULL)
        reduced_oper->setCurrent(&j);

}

void ResistiveMHDOperator::SetRHSEfield( double(* f)( const Vector&) ) 
{
   delete E0;
   delete E0rhs;
   E0rhs = new FunctionCoefficient(f);
   E0 = new ParLinearForm(&fespace);
   E0->AddDomainIntegrator(new DomainLFIntegrator(*E0rhs));
   E0->Assemble();
   E0Vec=E0->ParallelAssemble();

   //add E0 to reduced_oper
   if (reduced_oper!=NULL)
      reduced_oper->setE0(E0Vec);
}

void ResistiveMHDOperator::SetInitialJ(FunctionCoefficient initJ) 
{
    j.ProjectCoefficient(initJ);
    j.SetTrueVector();
    j.SetFromTrueVector();

    //add current to reduced_oper
    if (reduced_oper!=NULL)
        reduced_oper->setCurrent(&j);
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

   //compute the current as an auxilary variable
   Vector &k_ = const_cast<Vector &>(vx);
   gftmp.MakeTRef(&fespace, k_, sc);
   gftmp.SetFromTrueVector();  //recover psi

   Vector J, Z;
   HypreParMatrix A;
   KB->Mult(gftmp, zFull);
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

void ResistiveMHDOperator::ImplicitSolve(const double dt,
                                         const Vector &vx, Vector &k)
{
   int sc = height/3;
   Vector phi(vx.GetData() +   0, sc);
   Vector psi(vx.GetData() +  sc, sc);
   Vector   w(vx.GetData() +2*sc, sc);

   reduced_oper->SetParameters(dt, &phi, &psi, &w);
   Vector zero; // empty vector is interpreted as zero r.h.s. by NewtonSolver
   
   k = vx; //Provide the initial guess as vx and use iterative_mode
   pnewton_solver->Mult(zero, k);  //here k is solved as vx^{n+1}
   MFEM_VERIFY(pnewton_solver->GetConverged(),
                  "Newton solver did not converge.");
   //modify k so that it fits into the backward euler framework
   k-=vx;
   k/=dt;

   bool output=false;
   if (output)
   {
      ParGridFunction phi1, psi1, w1;
      phi1.MakeTRef(&fespace, k, 0);
      psi1.MakeTRef(&fespace, k, sc);
        w1.MakeTRef(&fespace, k, 2*sc);

      phi1.SetFromTrueVector(); psi1.SetFromTrueVector(); w1.SetFromTrueVector();

      ostringstream phi_name, psi_name, w_name;
      int myid;
      if (myid==0)
          cout <<"======OUTPUT: matrices in ResistiveMHDOperator:ImplicitSolve======"<<endl;
      MPI_Comm_rank(MPI_COMM_WORLD, &myid);
      phi_name << "dbg_phi." << setfill('0') << setw(6) << myid;
      psi_name << "dbg_psi." << setfill('0') << setw(6) << myid;
      w_name << "dbg_omega." << setfill('0') << setw(6) << myid;

      ofstream osol(phi_name.str().c_str());
      osol.precision(8);
      phi1.Save(osol);

      ofstream osol3(psi_name.str().c_str());
      osol3.precision(8);
      psi1.Save(osol3);

      ofstream osol4(w_name.str().c_str());
      osol4.precision(8);
      w1.Save(osol4);
   }
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

void ResistiveMHDOperator::UpdateJ(Vector &k, ParGridFunction *jout)
{
   //the current is J=-M^{-1}*K*Psi
   int sc = height/3;

   Vector &k_ = const_cast<Vector &>(k);
   gftmp.MakeTRef(&fespace, k_, sc);
   gftmp.SetFromTrueVector();

   Vector J, Z;
   HypreParMatrix A;
   KB->Mult(gftmp, zFull);
   zFull.Neg(); // z = -z
   M->FormLinearSystem(ess_tdof_list, j, zFull, A, J, Z); //apply Dirichelt boundary 
   M_solver.Mult(Z, J);
   M->RecoverFEMSolution(J, zFull, *jout);
}

void ResistiveMHDOperator::DestroyHypre()
{
    //hypre and petsc needs to be deleted earilier
    delete K_amg;
    delete M_prec;
    delete K_prec;
    delete reduced_oper;
    delete J_factory;
    delete pnewton_solver;
}

ResistiveMHDOperator::~ResistiveMHDOperator()
{
    //free used memory
    delete M;
    delete K;
    delete E0;
    delete E0Vec;
    delete E0rhs;
    delete Sw;
    delete KB;
    delete Nv;
    delete Nb;
    delete K_pcg;
    delete DRetmp;
    delete DSltmp;
}

ReducedSystemOperator::ReducedSystemOperator(ParFiniteElementSpace &f,
   ParBilinearForm *M_, HypreParMatrix &Mmat_,
   ParBilinearForm *K_, HypreParMatrix &Kmat_,
   ParBilinearForm *KB_, ParBilinearForm *DRe_, ParBilinearForm *DSl_,
   CGSolver *M_solver_,
   const Array<int> &ess_tdof_list_, const Array<int> &ess_bdr_)
   : Operator(3*f.TrueVSize()), fespace(f), 
     M(M_), K(K_), KB(KB_), DRe(DRe_), DSl(DSl_), Mmat(Mmat_), Kmat(Kmat_), 
     initialMdt(false), E0Vec(NULL), M_solver(M_solver_),
     dt(0.0), dtOld(0.0), phi(NULL), psi(NULL), w(NULL), 
     ess_tdof_list(ess_tdof_list_),ess_bdr(ess_bdr_),
     Nv(NULL), Nb(NULL), Pw(NULL), PB_VPsi(NULL), PB_VOmega(NULL), PB_BJ(NULL),
     Jacobian(NULL), z(height/3), zFull(f.GetVSize())
{ 
    useFull=0;
    MPI_Comm_rank(MPI_COMM_WORLD, &myid);

    //the following is not right because Mdtpr shares the same matrix with Mmat_
    //hypre_ParCSRMatrix *csrM = (hypre_ParCSRMatrix*)(Mmat_);
    //Mdtpr = new HypreParMatrix(csrM, true);

    //correct way to deep copy:
    Mdtpr = new HypreParMatrix(Mmat_);

    int sc = height/3;
    block_trueOffsets.SetSize(4);
    block_trueOffsets[0] = 0;
    block_trueOffsets[1] = sc;
    block_trueOffsets[2] = 2*sc;
    block_trueOffsets[3] = 3*sc;
}

ReducedSystemOperator::ReducedSystemOperator(ParFiniteElementSpace &f,
   ParBilinearForm *M_, HypreParMatrix &Mmat_,
   ParBilinearForm *K_, HypreParMatrix &Kmat_,
   ParBilinearForm *KB_, 
   ParBilinearForm *DRe_, HypreParMatrix *DRemat_,
   ParBilinearForm *DSl_, HypreParMatrix *DSlmat_,
   CGSolver *M_solver_,const Array<int> &ess_tdof_list_,
   const Array<int> &ess_bdr_, int useFull_)
   : Operator(3*f.TrueVSize()), fespace(f), 
     M(M_), K(K_), KB(KB_), DRe(DRe_), DSl(DSl_), Mmat(Mmat_), Kmat(Kmat_), 
     initialMdt(false),
     E0Vec(NULL), M_solver(M_solver_), 
     dt(0.0), dtOld(0.0), phi(NULL), psi(NULL), w(NULL), 
     ess_tdof_list(ess_tdof_list_), ess_bdr(ess_bdr_),
     Nv(NULL), Nb(NULL), Pw(NULL),  PB_VPsi(NULL), PB_VOmega(NULL), PB_BJ(NULL),
     Jacobian(NULL), z(height/3), zFull(f.GetVSize())
{ 
    useFull = useFull_;
    MPI_Comm_rank(MPI_COMM_WORLD, &myid);

    Mdtpr = new HypreParMatrix(Mmat_);
    ARe=NULL; ASl=NULL;

    DRematpr = DRemat_;
    DSlmatpr = DSlmat_;

    AReFull=NULL; ScFull=NULL; NbFull=NULL; PwMat=NULL; NbMat=NULL;

    MassIntegrator *mass = new MassIntegrator;
    Mlp = new ParBilinearForm(&fespace);
    Mlp->AddDomainIntegrator(new LumpedIntegrator(mass));
    Mlp->Assemble();
    Mlp->FormSystemMatrix(ess_tdof_list, Mmatlp);

    int sc = height/3;
    block_trueOffsets.SetSize(4);
    block_trueOffsets[0] = 0;
    block_trueOffsets[1] = sc;
    block_trueOffsets[2] = 2*sc;
    block_trueOffsets[3] = 3*sc;
}

/*
 * the full preconditioner is (note the sign of Nb)
 * [  ARe Nb  (Mlp)]
 * [  Pw  Sc  0    ]
 * [  K   0   M    ]
*/

Operator &ReducedSystemOperator::GetGradient(const Vector &k) const
{
   MFEM_ASSERT(initialMdt, "Mdt not initialized correctly!"); 

   if (useFull==1)
   {
       delete Jacobian;
       delete AReFull; 
       delete ScFull; 
       delete NbFull;
       delete PwMat;

       Vector &k_ = const_cast<Vector &>(k);

       int sc = height/3;
       //form Nv matrix
       delete Nv;
       phiGf.MakeTRef(&fespace, k_, 0);
       phiGf.SetFromTrueVector();
       Nv = new ParBilinearForm(&fespace);
       MyCoefficient velocity(&phiGf, 2);   //we update velocity
       Nv->AddDomainIntegrator(new ConvectionIntegrator(velocity));
       Nv->Assemble(); 
       Nv->EliminateEssentialBC(ess_bdr, Matrix::DIAG_ZERO);
       Nv->Finalize();
       HypreParMatrix *NvMat = Nv->ParallelAssemble();

       //change AReFull to the true ARe operator and ScFull to the true ASl operator
       AReFull = ParAdd(ARe, NvMat);
       HypreParMatrix *ASltmp = ParAdd(ASl, NvMat);    

       //form Nb matrix
       delete Nb;
       psiGf.MakeTRef(&fespace, k_, sc);
       psiGf.SetFromTrueVector();
       Nb = new ParBilinearForm(&fespace);
       MyCoefficient Bfield(&psiGf, 2);   //we update B
       Nb->AddDomainIntegrator(new ConvectionIntegrator(Bfield));
       Nb->Assemble();
       Nb->EliminateEssentialBC(ess_bdr, Matrix::DIAG_ZERO);
       Nb->Finalize();
       NbFull = Nb->ParallelAssemble();

       //form Pw operator        
       delete Pw;
       wGf.MakeTRef(&fespace, k_, 2*sc);
       wGf.SetFromTrueVector();
       Pw = new ParBilinearForm(&fespace);
       MyCoefficient curlw(&wGf, 2);
       Pw->AddDomainIntegrator(new ConvectionIntegrator(curlw));
       Pw->Assemble();
       Pw->EliminateEssentialBC(ess_bdr, Matrix::DIAG_ZERO);
       Pw->Finalize();
       PwMat = Pw->ParallelAssemble();

       //here we use B^T D^-1 B = (D^-1 B)^T B
       HypreParMatrix *DinvNb = new HypreParMatrix(*NbFull);
       HypreParVector *ARed = new HypreParVector(AReFull->GetComm(), AReFull->GetGlobalNumRows(),
                                        AReFull->GetRowStarts());
       HypreParMatrix *NbtDinv=NULL, *S=NULL;
       HypreParMatrix *tmp=Mdtpr;  //if use lumped matrix, it needs to be scaled by dt

       int iSc=0;
       if (iSc==0)
       {
           //VERSION0: same as Luis's preconditioner
           AReFull->GetDiag(*ARed);
           DinvNb->InvScaleRows(*ARed);
           NbtDinv=DinvNb->Transpose();
           S = ParMult(NbtDinv, NbFull);
           ScFull = ParAdd(ASltmp, S);
       }
       else if (iSc==1)
       {
           //VERSION1: schur complement without transpose
           //here Sc=ASl-B D^-1 B 
           AReFull->GetDiag(*ARed);
           DinvNb->InvScaleRows(*ARed);
           S = ParMult(NbFull, DinvNb);
           *S *= -1;
           ScFull = ParAdd(ASltmp, S);
       }
       else if (iSc==2) {
           //VERSION2: use (lumped) mass matrix
           if (myid==0) 
           {
              cout <<"======WARNING: use scaled mass matrix in Schur complement======"<<endl;
              cout <<"======WARNING: this changes preconditioner in pcshell !!!======"<<endl;
           }
           tmp->GetDiag(*ARed);
           DinvNb->InvScaleRows(*ARed);
           NbtDinv=DinvNb->Transpose();
           S = ParMult(NbtDinv, NbFull);
           ScFull = ParAdd(ASltmp, S);
       }

       bool outputMatrix=false;
       if (outputMatrix)
       {
           if (myid==0) 
              cout <<"======OUTPUT: matrices in ReducedSystemOperator:GetGradient======"<<endl;
           ofstream myf ("DRe.m");
           DRematpr->PrintMatlab(myf);

           ofstream myfile ("ARe.m");
           ARe->PrintMatlab(myfile);

           ofstream myfile0 ("AReFull.m");
           AReFull->PrintMatlab(myfile0);

           ofstream myfile2 ("NvMat.m");
           NvMat->PrintMatlab(myfile2);

           ofstream myfile4 ("lump.m");
           Mmatlp.PrintMatlab(myfile4);

           ARed->Print("diag.dat");
       }

       delete DinvNb;
       delete ARed;
       delete NbtDinv;
       delete S;
       delete NvMat;
       delete ASltmp;

       Jacobian = new BlockOperator(block_trueOffsets);
       Jacobian->SetBlock(0,0,AReFull);
       Jacobian->SetBlock(0,1,NbFull);
       Jacobian->SetBlock(1,0,PwMat);
       Jacobian->SetBlock(1,1,ScFull);
       Jacobian->SetBlock(2,0,&Kmat);
       Jacobian->SetBlock(2,2,&Mmat);

       if (iSc==2) Jacobian->SetBlock(0,2,tmp);
   }
   else
   {
      if (Jacobian == NULL)    //in the first pass we just set Jacobian once
      {
         Jacobian = new BlockOperator(block_trueOffsets);
         Jacobian->SetBlock(0,0,&Kmat);
         Jacobian->SetBlock(0,2,&Mmat);
         Jacobian->SetBlock(1,1,Mdtpr);
         Jacobian->SetBlock(2,2,Mdtpr);
      }
   }

   return *Jacobian;
}

ReducedSystemOperator::~ReducedSystemOperator()
{
   delete Mdtpr;
   delete ARe;
   delete ASl;
   delete AReFull;
   delete ScFull;
   delete NbFull;
   delete PwMat;
   delete NbMat;
   delete Jacobian;
   delete Nv;
   delete Nb;
   delete Pw;
   delete PB_VPsi; 
   delete PB_VOmega;
   delete PB_BJ;
   delete Mlp;
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

   Vector &k_ = const_cast<Vector &>(k);
   phiGf.MakeTRef(&fespace, k_, 0);
   phiGf.SetFromTrueVector();
   psiGf.MakeTRef(&fespace, k_, sc);
   psiGf.SetFromTrueVector();

   //two different ways to implement Poission Bracket
   //BilinearForm seems a better idea unless we are willing to 
   //sacrifice the accuracy (use a less accurate integrator)
   bool bilinearPB = true;
   if (bilinearPB)
   {
      //------assemble Nv and Nb (operators are assembled locally)------
      delete Nv;
      Nv = new ParBilinearForm(&fespace);
      MyCoefficient velocity(&phiGf, 2);   //we update velocity
      Nv->AddDomainIntegrator(new ConvectionIntegrator(velocity));
      Nv->Assemble(); 
   }
   else
   {
      delete PB_VPsi;
      PB_VPsi = new ParLinearForm(&fespace);
      PBCoefficient pbCoeff(&phiGf, &psiGf);
      //intOrder = 3*k+0
      PB_VPsi->AddDomainIntegrator(new DomainLFIntegrator(pbCoeff, 3, 0));
      PB_VPsi->Assemble();

      wGf.MakeTRef(&fespace, k_, 2*sc);
      wGf.SetFromTrueVector();

      delete PB_VOmega;
      PB_VOmega = new ParLinearForm(&fespace);
      PBCoefficient pbCoeff2(&phiGf, &wGf);
      PB_VOmega->AddDomainIntegrator(new DomainLFIntegrator(pbCoeff2, 3, 0));
      PB_VOmega->Assemble();
   }

   //------compute the current as an auxilary variable------
   Vector J, Z;
   HypreParMatrix A;
   KB->Mult(psiGf, zFull);
   zFull.Neg(); // z = -z
   M->FormLinearSystem(ess_tdof_list, *j0, zFull, A, J, Z); //apply Dirichelt boundary 
   M_solver->Mult(Z, J); 

   //+++++compute y1
   Kmat.Mult(phiNew,y1);
   Mmat.Mult(wNew,z);
   y1+=z;
   y1.SetSubVector(ess_tdof_list, 0.0);

   //+++++compute y2
   add(psiNew, -1., *psi, z);
   z/=dt;
   Mmat.Mult(z,y2);
   if (bilinearPB)
      Nv->TrueAddMult(psiNew,y2);
   else
      y2 += *PB_VPsi;

   if (DSl!=NULL)
       DSl->TrueAddMult(psiNew,y2);
   if (E0Vec!=NULL)
       y2 += *E0Vec;
   y2.SetSubVector(ess_tdof_list, 0.0);

   //+++++compute y3
   add(wNew, -1., *w, z);
   z/=dt;
   Mmat.Mult(z,y3);
   if (bilinearPB)
      Nv->TrueAddMult(wNew,y3);
   else
      y3 += *PB_VOmega;

   if (DRe!=NULL)
       DRe->TrueAddMult(wNew,y3);

   if (bilinearPB)
   {      
      delete Nb;
      Nb = new ParBilinearForm(&fespace);
      MyCoefficient Bfield(&psiGf, 2);   //we update B
      Nb->AddDomainIntegrator(new ConvectionIntegrator(Bfield));
      Nb->Assemble();
      if (true)
         Nb->TrueAddMult(J, y3, -1.); 
      else
      {
          delete NbMat;
          Nb->Finalize();
          NbMat=Nb->ParallelAssemble();
          //NbMat->Mult(-1., J, 1., y3);
          NbMat->MultTranspose(1., J, 1., y3);
      }
 
   }
   else
   {
      //we let J=-J for applying -Nb*J
      J.Neg();
      //use wGf to temporarily hold j
      wGf.MakeTRef(&fespace, J, 0);
      wGf.SetFromTrueVector();
      delete PB_BJ;
      PB_BJ = new ParLinearForm(&fespace);
      PBCoefficient pbCoeff(&psiGf, &wGf);

      PB_BJ->AddDomainIntegrator(new DomainLFIntegrator(pbCoeff, 3, 0));
      PB_BJ->Assemble();
      HypreParVector *trueBJ = PB_BJ->ParallelAssemble();
      y3 += *trueBJ;
      delete trueBJ;
   }
   y3.SetSubVector(ess_tdof_list, 0.0);
}
