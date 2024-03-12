#include <iostream>
#include <algorithm>
#include <cstring>
using namespace std;

#include <cblas.h>
#include <mpi.h>

#include "SolverCG.h"

/**
 * @brief Macro to map matrix entry i,j onto it's corresponding location in memory, assuming column-wise matrix storage
 * @param I     matrix index i denoting the ith row
 * @param J     matrix index j denoting the jth columns
 */
#define IDX(I,J) ((J)*Nx + (I))                     //define a new operation to improve computation?

SolverCG::SolverCG(int pNx, int pNy, double pdx, double pdy,MPI_Comm &rowGrid, MPI_Comm &colGrid)
{
    dx = pdx;
    dy = pdy;
    Nx = pNx;
    Ny = pNy;
    int n = Nx*Ny;                                  //total number of grid points
    r = new double[n];                              //allocate arrays size with total number of grid points
    p = new double[n];
    z = new double[n];
    t = new double[n];
    
    //MPI communication stuff
    comm_row_grid = rowGrid;
    comm_col_grid = colGrid;
    MPI_Comm_size(comm_row_grid,&size);     //get size of communicator

    //compute ranks along the row communciator and along teh column communicator
    MPI_Comm_rank(comm_row_grid, &rowRank); 
    MPI_Comm_rank(comm_col_grid, &colRank);

    //compute ranks for adjacent grids for data transfer, if at boundary, returns -2 (MPI_PROC_NULL)
    MPI_Cart_shift(comm_col_grid,0,1,&topRank,&bottomRank);
    MPI_Cart_shift(comm_row_grid,0,1,&leftRank,&rightRank);
    
    if((topRank != MPI_PROC_NULL) & (bottomRank != MPI_PROC_NULL) & (leftRank != MPI_PROC_NULL) & (rightRank != MPI_PROC_NULL))
        boundaryDomain = false;
    else
        boundaryDomain = true;      //check whether the current process is on the edge of the grid/cavity    
}

SolverCG::~SolverCG()
{
    delete[] r;
    delete[] p;
    delete[] z;
    delete[] t;
}

//getter functions for testing purposes
double SolverCG::GetDx() {
    return dx;
}

double SolverCG::GetDy() {
    return dy;
}

int SolverCG::GetNx() {
    return Nx;
}

int SolverCG::GetNy() {
    return Ny;
}

void SolverCG::Solve(double* b, double* x) {
    unsigned int n = Nx*Ny;                         //total number of grid points
    int k;                                          //counter to track iteration number
    double alpha;                                   //variables for conjugate gradient algorithm
    double beta;
    double eps;                                     //error                                     !! error or error squared !!
    double tol = 0.001;                             //error tolerance

    eps = cblas_dnrm2(n, b, 1);                     //if 2-norm of b is lower than tolerance squared, then b practically zero
    if (eps < tol*tol) {                        
        std::fill(x, x+n, 0.0);                     //hence solution x is practically 0, output the 2-norm and exit function
        cout << "Norm is " << eps << endl;          //don't waste time with algorithm
        return;
    }
    
    // --------------------------- PRECONDITIONED CONJUGATE GRADIENT ALGORITHM ---------------------------------------------------//
    //Refer to standard notation provided in the literature for this algorithm 
    
    ApplyOperator(x, t);                            //apply discretised operator -nabla^2 to x, so t = -nabla^2 x, or t = Ax 
    cblas_dcopy(n, b, 1, r, 1);                     // r_0 = b (i.e. b)
    ImposeBC(r);                                    //apply zeros to edges

    cblas_daxpy(n, -1.0, t, 1, r, 1);               //r=r-t (i.e. r = b - Ax) gives first step of conjugate gradient algorithm
    Precondition(r, z);                             //Precondition the problem, preconditioned matrix in z
    cblas_dcopy(n, z, 1, p, 1);                     // p_0 = z_0 (where z_0 is the preconditioned version of r_0)

    k = 0;                                          //initialise iteration counter
    do {
        k++;
        
        ApplyOperator(p, t);                        //compute -nabla^2 p and store in t (effectively A*p_k)

        alpha = cblas_ddot(n, t, 1, p, 1);          // alpha = p_k^T*A*p_k
        alpha = cblas_ddot(n, r, 1, z, 1) / alpha;  // compute alpha_k = (r_k^T*r_k) / (p_k^T*A*p_k)
        beta  = cblas_ddot(n, r, 1, z, 1);          // z_k^T*r_k (for later in the algorithm)

        cblas_daxpy(n,  alpha, p, 1, x, 1);         // x_{k+1} = x_k + alpha_k*p_k
        cblas_daxpy(n, -alpha, t, 1, r, 1);         // r_{k+1} = r_k - alpha_k*A*p_k

        eps = cblas_dnrm2(n, r, 1);                 //norm r_{k+1} is error between algorithm and solution, check error tolerance

        if (eps < tol*tol) {
            break;                                  //stop algorithm if solution is within specified tolerance
        }
        Precondition(r, z);                         //precondition r_{k+1} and store in z_{k+1}
        beta = cblas_ddot(n, r, 1, z, 1) / beta;    //compute beta_k = (r_{k+1}^T*r_{k+1}) / (r_k^T*r_k)

        cblas_dcopy(n, z, 1, t, 1);                 //copy z_{k+1} into t, so t now holds preconditioned r_{k+1}
        cblas_daxpy(n, beta, p, 1, t, 1);           //t = t + beta_k*p_k i.e. p_{k+1} = z_{k+1} + beta_k*p_k
        cblas_dcopy(n, t, 1, p, 1);                 //copy z_{k+1} from t into p, so p_{k+1} = z{k+1}, ready for next iteration

    } while (k < 5000);                             // max 5000 iterations

    if (k == 5000) {
        cout << "FAILED TO CONVERGE" << endl;
        exit(-1);                                   //if 5000 iterations reached, no converged solution, so terminate program
    }                                               //otherwise, output results and details from the algorithm

    cout << "Converged in " << k << " iterations. eps = " << eps << endl;
}

void SolverCG::ApplyOperator(double* in, double* out) {
    // Assume ordered with y-direction fastest (column-by-column)
    double dx2i = 1.0/dx/dx;                                    //optimised code, compute and store 1/(dx)^2 and 1/(dy)^2
    double dy2i = 1.0/dy/dy;
    int jm1 = 0, jp1 = 2;                                       //jm1 is j-1, jp1 is j+1; this allows for vectorisation of operation
    for (int j = 1; j < Ny - 1; ++j) {                          //compute this only for interior grid points
        for (int i = 1; i < Nx - 1; ++i) {                      //i denotes x grids, j denotes y grids
            out[IDX(i,j)] = ( -     in[IDX(i-1, j)]             //note predefinition of IDX(i,j) = j*Nx + i, which yields location of entry i,j in memory
                              + 2.0*in[IDX(i,   j)]
                              -     in[IDX(i+1, j)])*dx2i       //calculates part of equation for second-order central-difference related to x
                          + ( -     in[IDX(i, jm1)]
                              + 2.0*in[IDX(i,   j)]
                              -     in[IDX(i, jp1)])*dy2i;      //calculates part of equation for second-order central-difference related to y
        }
        jm1++;                                                  //jm1 and jp1 incremented separately outside i loop
        jp1++;                                                  //this is done instead of j-1,j+1 in inner loop to encourage vectorisation
    }
}

void SolverCG::Precondition(double* in, double* out) {
    int i, j;
    double dx2i = 1.0/dx/dx;
    double dy2i = 1.0/dy/dy;                                //optimised code, compute and store 1/(dx)^2 and 1/(dy)^2
    double factor = 2.0*(dx2i + dy2i);                      //precondition will involve dividing all terms by 2(1/dx/dx + 1/dy/dy)
    
    //first do process corners
    if( leftRank == MPI_PROC_NULL | bottomRank == MPI_PROC_NULL) {//if process is on the left or bottom, impose BC on bottom left corner 
        out[0] = in[0];
    }
    else {//otherwise, precondition
        out[0] = in[0]/factor;
    }
    
    if( leftRank == MPI_PROC_NULL | topRank == MPI_PROC_NULL) { //if process is on left ro top, impose BC on top left corner
        out[IDX(0,Ny-1)] = in[IDX(0,Ny-1)];
    }
    else{
        out[IDX(0,Ny-1)] = in[IDX(0,Ny-1)]/factor;
    }
    
    if( rightRank == MPI_PROC_NULL | bottomRank == MPI_PROC_NULL) {//if process is on right or bottom, impose BC on bottom right corner
        out[IDX(Nx-1,0)] = in[IDX(Nx-1,0)];
    }
    else{
        out[IDX(Nx-1,0)] = in[IDX(Nx-1,0)]/factor;        
    }    
    
    if(rightRank == MPI_PROC_NULL | topRank == MPI_PROC_NULL) {//if process is on right or top, impose BC on top right corner
        out[IDX(Nx-1,Ny-1)] = in[IDX(Nx-1,Ny-1)];
    }
    else{
        out[IDX(Nx-1,Ny-1)] = in[IDX(Nx-1,Ny-1)]/factor;
    }
    
    //now compute for process edges
    if( leftRank != MPI_PROC_NULL) {        //if process not on left boundary, precodnition LHS, otherwise maintain same BC
        for(j = 1; j < Ny-1; ++j) {
            out[IDX(0,j)] = in[IDX(0,j)]/factor;
        }
    }
    else {
        for(j = 1; j < Ny-1; ++j) {
            out[IDX(0,j)] = in[IDX(0,j)];
        }
    }
    
    if(rightRank != MPI_PROC_NULL) {//if process not on right boundary, precodnition RHS, otherwise maintain same BC
        for(j = 1; j < Ny - 1; ++j) {
            out[IDX(Nx-1,j)] = in[IDX(Nx-1,j)]/factor;
        }
    }
    else {
        for(j = 1; j < Ny - 1; ++j) {
            out[IDX(Nx-1,j)] = in[IDX(Nx-1,j)];
        }
    }
    
    if(bottomRank != MPI_PROC_NULL) {   //if process not on bottom boundary, precodntion bottom row, otherwise maintain same BC
        for(i = 1; i < Nx - 1; ++i) {
            out[IDX(i,0)] = in[IDX(i,0)]/factor;
        }
    }
    else {
         for(i = 1; i < Nx - 1; ++i) {
            out[IDX(i,0)] = in[IDX(i,0)];
        }
    }
    
    if(topRank != MPI_PROC_NULL) {   //if process not on top boundary, precodntion top row, otherwise maintain same BC
        for(i = 1; i < Nx - 1; ++i) {
            out[IDX(i,Ny-1)] = in[IDX(i,Ny-1)]/factor;
        }
    }
    else {
         for(i = 1; i < Nx - 1; ++i) {
            out[IDX(i,Ny-1)] = in[IDX(i,Ny-1)];
        }
    }
    
    //precondition all other interior points
    for (i = 1; i < Nx - 1; ++i) {
        for (j = 1; j < Ny - 1; ++j) {                  
            out[IDX(i,j)] = in[IDX(i,j)]/factor;            //apply precondition, reduce condition number
        }
    }
}

void SolverCG::ImposeBC(double* inout) {
        
    
    
    // Boundaries
    for (int i = 0; i < Nx; ++i) {
        inout[IDX(i, 0)] = 0.0;                         //zero BC on bottom surface
        inout[IDX(i, Ny-1)] = 0.0;                      //zero BC on top surface
    }

    for (int j = 0; j < Ny; ++j) {
        inout[IDX(0, j)] = 0.0;                         //zero BC on left surface
        inout[IDX(Nx - 1, j)] = 0.0;                    //zero BC on right surface
    }
}