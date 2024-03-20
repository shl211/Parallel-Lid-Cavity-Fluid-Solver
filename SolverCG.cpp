#include <iostream>
#include <algorithm>
#include <cstring>
#include <cmath>
using namespace std;

#include <cblas.h>
#include <mpi.h>
#include <omp.h>

#include "SolverCG.h"

/**
 * @brief Macro to map coordinates (i,j) onto it's corresponding location in memory, assuming row-wise matrix storage
 * @param I     coordinate i denoting horizontal position of grid from left to right
 * @param J     coordinate j denoting vertical position of grid from bottom to top
 */
#define IDX(I,J) ((J)*Nx + (I))

/*
    It is assumed that the problem domain passed to SolverCG is already discretised into its local domain on each process
    Henceforth, references to LOCAL refer to the local domain and values stored on each value, 
    while references to GLOBAL refer to the global domain and values that describe the unsplit problem
*/

SolverCG::SolverCG(int pNx, int pNy, double pdx, double pdy,MPI_Comm &rowGrid, MPI_Comm &colGrid)
{
    //SolverCG expects domain values to be already discretised, so all member variables are local unless otherwise stated
    dx = pdx;
    dy = pdy;
    Nx = pNx;
    Ny = pNy;
    int n = Nx*Ny;                                  //total number of local grid points
    r = new double[n];                              //allocate arrays size with total number of grid points
    p = new double[n];
    z = new double[n];
    t = new double[n];
    
    topData = new double[Nx];
    bottomData = new double[Nx];
    leftData = new double[Ny];                      //store data from other processes here
    rightData = new double[Ny];
    
    //temp data storage for receviing -> don't want send receive buffers to be same to prevent accidental overwrite
    tempLeft = new double[Ny];
    tempRight = new double[Ny];

    //extract some useful data from MPI communicators
    comm_row_grid = rowGrid;
    comm_col_grid = colGrid;

    MPI_Comm_size(comm_row_grid,&size);             //get size of communicator -> number of processes along each dimension
    MPI_Comm_rank(comm_row_grid, &rowRank);         //compute current rank along row and column communicators
    MPI_Comm_rank(comm_col_grid, &colRank);

    //compute ranks for adjacent grids for data transfer, if at boundary, returns -2 (MPI_PROC_NULL), a dummy process
    MPI_Cart_shift(comm_col_grid,0,1,&bottomRank,&topRank);                     //from bottom to top
    MPI_Cart_shift(comm_row_grid,0,1,&leftRank,&rightRank);                     //from left to right
    
    if((topRank != MPI_PROC_NULL) & (bottomRank != MPI_PROC_NULL) & (leftRank != MPI_PROC_NULL) & (rightRank != MPI_PROC_NULL))
        boundaryDomain = false;
    else
        boundaryDomain = true;                      //check whether the current process is on the edge of the global domain/grid

    
}

SolverCG::~SolverCG()
{
    delete[] r;
    delete[] p;
    delete[] z;
    delete[] t;
    
    delete[] leftData;
    delete[] rightData;
    delete[] topData;
    delete[] bottomData;

    delete[] tempLeft;
    delete[] tempRight;
}

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
    unsigned int n = Nx*Ny;                         //total number of local grid points
    int k;                                          //counter to track iteration number
    double alphaNum;                                //local variables for conjugate gradient algorithm
    double alphaDen;
    double betaNum;
    double betaDen;
    double eps;                                     //error
    double tol = 0.001;                             //error tolerance
    
    //global variables
    double globalAlpha;
    double globalAlphaTemp;
    double globalBeta;
    double globalBetaTemp;
    double globalEps;

    eps = cblas_dnrm2(n, b, 1);
    eps *= eps;                                     //calculate and square error for summation as 2-norm can't be summed by 2-normed squared can 

    //need to compute sum of error norms across all process for comparison, don't use local
    MPI_Allreduce(&eps,&globalEps,1,MPI_DOUBLE,MPI_SUM,MPI_COMM_WORLD);
    globalEps = sqrt(globalEps);                    //reduction gives norm squared, so sqrt it to get 2-norm of the 'global'/actual error

    if (globalEps < tol*tol) {                      //if 2-norm of b is lower than tolerance squared, then b practically zero
        std::fill(x, x+n, 0.0);                     //hence don't waste time with algorithm, solution x is 0
        if((rowRank == 0) & (colRank == 0))         //print on root rank only
            cout << "Norm is " << globalEps << endl;
        return;
    }
    
    // --------------------------- PRECONDITIONED CONJUGATE GRADIENT ALGORITHM ---------------------------------------------------//
    //Refer to standard notation provided in the literature for this algorithm
    ApplyOperator(x, t);                            //apply discretised operator -nabla^2 to x, so t = -nabla^2 x, or t = Ax 
    cblas_dcopy(n, b, 1, r, 1);                     // r_0 = b (i.e. b)
    ImposeBC(r);                                    //apply zeros to edges of global, not local, domain

    cblas_daxpy(n, -1.0, t, 1, r, 1);               //r=r-t (i.e. r = b - Ax) gives first step of conjugate gradient algorithm
    Precondition(r, z);                             //Precondition the problem, preconditioned matrix in z
    cblas_dcopy(n, z, 1, p, 1);                     // p_0 = z_0 (where z_0 is the preconditioned version of r_0)

    k = 0;
    
    do {
        k++;

        ApplyOperator(p, t);                        //compute -nabla^2 p and store in t (effectively A*p_k)

        //division cannot be performed locally then summed, numerator and denominator must be summed separately to get global numerator and denominator
        //(that describes the ACTUAL alpha of the problem) then divided for global alpha (and beta) 

        alphaDen = cblas_ddot(n, t, 1, p, 1);                                               // denominator of alpha = p_k^T*A*p_k (^T is transpose)
        alphaNum = cblas_ddot(n, r, 1, z, 1);                                               // numerator of alpha = r^k^T*r_k              
        betaDen  = cblas_ddot(n, r, 1, z, 1);                                               // denominator of beta = z_k^T*r_k (for later in the algorithm)
            
        MPI_Allreduce(&alphaDen,&globalAlphaTemp,1,MPI_DOUBLE,MPI_SUM,MPI_COMM_WORLD);      //sum up local alpha denominators
        MPI_Allreduce(&alphaNum,&globalAlpha, 1, MPI_DOUBLE, MPI_SUM,MPI_COMM_WORLD);       //sum up local alpha numerators

        globalAlpha = globalAlpha/globalAlphaTemp;                                          //compute global(actual) alpha_k = (r_k^T*r_k) / (p_k^T*A*p_k)

        //no parallel region here as tasks is slower than just letting BLAS do its thing
        cblas_daxpy(n,  globalAlpha, p, 1, x, 1);                                           // x_{k+1} = x_k + alpha_k*p_k
        cblas_daxpy(n, -globalAlpha, t, 1, r, 1);                                           // r_{k+1} = r_k - alpha_k*A*p_k
    
        eps = cblas_dnrm2(n, r, 1);                                                         //norm r_{k+1} is error between algorithm and solution
        eps *= eps;

        MPI_Allreduce(&eps,&globalEps,1,MPI_DOUBLE,MPI_SUM,MPI_COMM_WORLD);
        globalEps = sqrt(globalEps);

        if (globalEps < tol*tol) {
            break;                                                                          //stop algorithm if solution converges within specified tolerance
        }
        
        Precondition(r, z);                                                                 //precondition r_{k+1} and store in z_{k+1}

        betaNum = cblas_ddot(n, r, 1, z, 1);                                                //numerator of beta = (r_{k+1}^T*r_{k+1})
                
        cblas_dcopy(n, z, 1, t, 1);                                                         //copy z_{k+1} into t, so t now holds preconditioned r_{k+1}
        
        MPI_Allreduce(&betaDen,&globalBetaTemp,1,MPI_DOUBLE,MPI_SUM,MPI_COMM_WORLD);
        MPI_Allreduce(&betaNum,&globalBeta,1, MPI_DOUBLE,MPI_SUM,MPI_COMM_WORLD);
        
        globalBeta = globalBeta / globalBetaTemp;                                           //global/actual beta = (r_{k+1}^T*r_{k+1}) / (r_k^T*r_k)         

        cblas_daxpy(n, globalBeta, p, 1, t, 1);                                             //t = t + beta_k*p_k i.e. p_{k+1} = z_{k+1} + beta_k*p_k
        cblas_dcopy(n, t, 1, p, 1);                                                         //copy z_{k+1} from t into p, so p_{k+1} = z{k+1}, for next iteration
    } while (k < 5000);

    if (k == 5000) {
        if((rowRank == 0) & (colRank == 0))
            cout << "FAILED TO CONVERGE" << endl;
        exit(-1);                                   //if max 5000 iterations reached, no converged solution, so terminate program
    }                                               //otherwise, output results and details from the algorithm

    if((rowRank == 0) & (colRank == 0))
        cout << "Converged in " << k << " iterations. eps = " << globalEps << endl;
    }

    
/* 
ApplyOperator requires five point stencil, so will need boundary data from adjacent grids.
To avoid latency, boundary data sent first, and while waiting for receive, do interior points first, which do not require other process data
Then receive boundary data, and compute corners followed by edges of each local domain. Corners and edges require access to differnet arrays
Could be simplified with packed storage, faster computations due to memory layout but this is lost if want to minimse communication latency 
via the method outlined above. Note that global BC will be implemented in ImposeBC()
*/
void SolverCG::ApplyOperator(double* in, double* out) {

    //-----------------------------------------------------------------------------------------------------------------------------------//
    //------------------------------------STEP 1: Send Boundary Data; Compute Interior Points while waiting to Receive-------------------//
    //-----------------------------------------------------------------------------------------------------------------------------------//
    //global boundary denoted by MPI_PROC_NULL, a dummy variable that returns immediately in communications
    
    MPI_Isend(in+Nx*(Ny-1), Nx, MPI_DOUBLE, topRank, 0, comm_col_grid,&requests[0]);        //send data on top of current process up -> tag 0
    MPI_Isend(in,Nx,MPI_DOUBLE,bottomRank,1,comm_col_grid,&requests[1]);                    //send data on bottom of current process down -> tag 1

    //now, extract relevant data for left and right columns and send, done after first sends to give time for above data to be sent to minimise latency
    cblas_dcopy(Ny,in,Nx,tempLeft,1);                                                       //use temp buffer to prevent accidental data overwrite with Isend
    cblas_dcopy(Ny,in+Nx-1,Nx,tempRight,1);

    MPI_Isend(tempLeft,Ny,MPI_DOUBLE,leftRank,2,comm_row_grid,&requests[2]);                //send data on LHS of current process to the left -> tag 2
    MPI_Isend(tempRight,Ny,MPI_DOUBLE, rightRank,3,comm_row_grid,&requests[3]);             //send data on RHS of current process to right -> tag 3
    
    //computing interior points on all local domains
    double dx2i = 1.0/dx/dx;
    double dy2i = 1.0/dy/dy;

    //dynamic scheduling for load balancing -> after testing, more effective than static
    #pragma omp parallel for schedule(dynamic) private(i,j)
        for (j = 1; j < Ny - 1; ++j) {
            for (i = 1; i < Nx - 1; ++i) {
                out[IDX(i,j)] = ( -     in[IDX(i-1, j)]
                                + 2.0*in[IDX(i,   j)]
                                -     in[IDX(i+1, j)])*dx2i
                            + ( -     in[IDX(i, j-1)]
                                + 2.0*in[IDX(i,   j)]
                                -     in[IDX(i, j+1)])*dy2i;
            }
        }

    //receive the data required for calculating boundary data of each local domain    
    MPI_Recv(bottomData,Nx,MPI_DOUBLE,bottomRank,0,comm_col_grid,MPI_STATUS_IGNORE);    //bottom row of process is data sent up from process below
    MPI_Recv(topData,Nx,MPI_DOUBLE,topRank,1,comm_col_grid, MPI_STATUS_IGNORE);         //top row of process is data sent down from process above
    MPI_Recv(rightData,Ny,MPI_DOUBLE,rightRank,2,comm_row_grid,MPI_STATUS_IGNORE);      //right column of process is data sent from process to right
    MPI_Recv(leftData,Ny,MPI_DOUBLE,leftRank,3,comm_row_grid,MPI_STATUS_IGNORE);        //left column of process is data sent from process to left
    
    //---------------------------------------------------------------------------------------------------------------------------------------------------//
    //---------------------------------------------Step 2: Compute Local Domain Corners -----------------------------------------------------------------//
    //---------------------------------------------------------------------------------------------------------------------------------------------------//
    //references to edges => edge of each local domain; references to boundary => boundary of global domain
    //first consider the unlikely edge cases where local domain is single cell, column vector, row vector
    
    //if local domain is single cell not on global boundary, then need access to data from four processes
    if((Nx == 1) &( Ny == 1) & !boundaryDomain) {
        out[0] = ( - leftData[0] + 2.0*in[0] - rightData[0] ) * dx2i
                + (- bottomData[0] + 2*in[0] - topData[0] ) * dy2i; 
    }
    else if((Nx == 1) & (Ny != 1) & !((leftRank == MPI_PROC_NULL) | (rightRank == MPI_PROC_NULL)) ) {
        //if local domain is effectively a colmumn vector, do this, unless it's at left or right boundary (BCs will be imposed there)
        //compute 'top' corner, unless at top grid boundary already
        if(topRank != MPI_PROC_NULL) {
            out[Ny-1] = (- leftData[Ny-1] + 2.0*in[Ny-1] - rightData[Ny-1] ) * dx2i
                    + (- in[Ny-2] + 2*in[Ny-1] - topData[0]) * dy2i; 
        }
        
        //compute 'bottom' corner, unless at bottom grid boundary already
        if(bottomRank != MPI_PROC_NULL) {
            out[0] = (- leftData[0] + 2.0 * in[0] - rightData[0] ) * dx2i
                    + (- bottomData[0] + 2*in[0] - in[1]) * dy2i;
        }
    }
    else if((Nx != 1) & (Ny == 1) & !((topRank == MPI_PROC_NULL) | (bottomRank == MPI_PROC_NULL)) ) {
        //if local domain effectively a row domain, do this, unless already at top or bottom boundaries (BCs will be imposed there)
        //compute 'left' corner, unless at LHS of grid already
        if(leftRank != MPI_PROC_NULL) {
            out[0] = ( - leftData[0] + 2.0 * in[0] - in[1] ) * dx2i
                    + (- bottomData[0] + 2*in[0] - topData[0]) * dy2i;
        }
        
        //compute 'right' corner, unless at RHS of grid already
        if(rightRank != MPI_PROC_NULL) {
            out[Nx-1] = (- in[Nx-2] + 2.0 * in[0] - rightData[1] ) * dx2i
                    +(- bottomData[Nx-1] + 2 * in[0] - topData[Nx-1] ) * dy2i;
        }
    } 
    else {
        //otherwise, for general case where only two datapoints from other processes are needded:
        //compute bottom left corner of domain, unless process is on left or bottom boundary, as already have BC there
       if(!((bottomRank == MPI_PROC_NULL) | (leftRank == MPI_PROC_NULL))) {
            out[IDX(0,0)] = (- leftData[0] + 2.0*in[IDX(0,0)] - in[IDX(1,0)]) * dx2i
                        + (- bottomData[0] + 2.0*in[IDX(0,0)] - in[IDX(0,1)]) * dy2i;
       }

        //compute bottom right corner of domain, unless process is on right or bottom boundary
        if(!((bottomRank == MPI_PROC_NULL) | (rightRank == MPI_PROC_NULL))) {
            out[IDX(Nx-1,0)] = (- in[IDX(Nx-2,0)] + 2.0*in[IDX(Nx-1,0)] - rightData[0]) * dx2i
                        + (- bottomData[Nx-1] + 2.0*in[IDX(Nx-1,0)] - in[IDX(Nx-1,1)]) * dy2i;
        }

        //compute top left corner of domain, unless process is on left or top boundary
        if(!((topRank == MPI_PROC_NULL) | (leftRank == MPI_PROC_NULL))) {
            out[IDX(0,Ny-1)] = (- leftData[Ny-1] + 2.0*in[IDX(0,Ny-1)] - in[IDX(1,Ny-1)]) * dx2i
                        + (- in[IDX(0,Ny-2)] + 2.0*in[IDX(0,Ny-1)] - topData[0]) * dy2i;
        }

        //compute top right corner of domain, unless process is on right or top boundary
        if(!((topRank == MPI_PROC_NULL) | (rightRank == MPI_PROC_NULL))) {
            out[IDX(Nx-1,Ny-1)] = (- in[IDX(Nx-2,Ny-1)] + 2.0*in[IDX(Nx-1,Ny-1)] - rightData[Ny-1]) * dx2i
                        + (- in[IDX(Nx-1,Ny-2)] + 2.0*in[IDX(Nx-1,Ny-1)] - topData[Nx-1]) * dy2i;
        }
    }
    
    //--------------------------------------------------------------------------------------------------------------------------//
    //----------------------------------Step 3 : Compute Local Domain Edges ----------------------------------------------------//
    //--------------------------------------------------------------------------------------------------------------------------//
    /*overheads associated with creating parallel region here exceeds any speed ups in the code
    for and sections were tested and gains were negligible in some cases but pretty much always resulted in worse performance
    Test case Lx,Ly=1, Nx,Ny=201,Re=1000,dt=0.005,T=0.1 were used for benchmark tests*/
        
    //unlikely edge cases require different data to be accessed, so do those first (row vector and column vector)
    //if column vector, don't need to do for left or right as BC already imposed along entire column
    if((Nx == 1) & (Ny > 1) & !((leftRank == MPI_PROC_NULL) | (rightRank == MPI_PROC_NULL))) {
        for(j = 1; j < Ny - 1; ++j) {
            out[j] = ( - leftData[j] + 2.0 * in[j] - rightData[j]) * dx2i
                    + ( -  in[j-1] + 2.0 * in[j] - in[j+1]) * dy2i;
        }
    }

    //if row vector, don't need to do for top and bottom rows as BC already imposed along entire row
    if((Nx != 1) & (Ny == 1) & !((topRank == MPI_PROC_NULL) | (bottomRank == MPI_PROC_NULL))) {
        for(i = 1; i < Nx - 1; ++i) {
            out[i] = ( - in[i-1] + 2.0 * in[i] - in[i+1] ) * dx2i
                    + ( - bottomData[i] + 2.0 * in[i] - topData[i]) * dy2i;
        }
    }

    //otherwise, for the general case, compute process edge data
    //only compute *** row if not at *** boundary of Cartesian grid (where ***Rank != MPI_PROC_NULL)
    if((Nx != 1) & (Ny != 1) & (bottomRank != MPI_PROC_NULL)) {
        for(i = 1; i < Nx - 1; ++i) {
            out[IDX(i,0)] = (- in[IDX(i-1,0)] + 2.0*in[IDX(i,0)] - in[IDX(i+1,0)] ) * dx2i
                        + ( - bottomData[i] + 2.0*in[IDX(i,0)] - in[IDX(i,1)] ) * dy2i;
        }
    }
        
    if((Nx != 1) & (Ny != 1) & (topRank != MPI_PROC_NULL)) {
        for(i = 1; i < Nx - 1; ++i) {
            out[IDX(i,Ny-1)] = (- in[IDX(i-1,Ny-1)] + 2.0*in[IDX(i,Ny-1)] - in[IDX(i+1,Ny-1)] ) * dx2i
                        + ( - in[IDX(i,Ny-2)] + 2.0 * in[IDX(i,Ny-1)] - topData[i]) * dy2i;
        }
    }
        
    if((Nx != 1) & (Ny != 1) & (leftRank != MPI_PROC_NULL)) {
        for(j = 1; j < Ny - 1; ++j) {
            out[IDX(0,j)] = (- leftData[j] + 2.0*in[IDX(0,j)] - in[IDX(1,j)] ) * dx2i
                        + ( - in[IDX(0,j-1)] + 2.0*in[IDX(0,j)] - in[IDX(0,j+1)] ) * dy2i;
        }
    }
            
    if((Nx != 1) & (Ny != 1) & (rightRank != MPI_PROC_NULL)) {
        for(j = 1; j < Ny - 1; ++j) {
            out[IDX(Nx-1,j)] = (- in[IDX(Nx-2,j)] + 2.0*in[IDX(Nx-1,j)] - rightData[j] ) * dx2i
                        + ( - in[IDX(Nx-1,j-1)] + 2.0*in[IDX(Nx-1,j)] - in[IDX(Nx-1,j+1)] ) * dy2i;
        }
    }
        
    //make sure all communication is completed before exiting the function
    MPI_Waitall(4,requests,MPI_STATUSES_IGNORE);
}

//procedure once again is compute interior points, edges, then corners
void SolverCG::Precondition(double* in, double* out) {

    double dx2i = 1.0/dx/dx;
    double dy2i = 1.0/dy/dy;
    double factor = 2.0*(dx2i + dy2i);                      //precondition will involve dividing all non-boundary terms by 2(1/dx/dx + 1/dy/dy)
    
    //here edge calculations also parallelised as parallel region already created for the nested O(n^2) loop
    //hence no overhead costs, so marginal gains can be made
    #pragma omp parallel private(i,j)
    {   
        //----------------------------------------------Step 1: Precondition Interior Points First--------------------------------------------//
        //dynamic for load balancing
        #pragma omp for schedule(dynamic) nowait
            for (j = 1; j < Ny - 1; ++j) {                  
                for (i = 1; i < Nx - 1; ++i) {
                    out[IDX(i,j)] = in[IDX(i,j)]/factor;
                }
            }
    
        //---------------------------------------------Step 2: Precondition Edges of each Local Domain ---------------------------------------//
        //lots of if statements, with each process computing four, so split the eight checks up between the processes as sections
        //sections rather than fors as fors were found to reduce performance
        #pragma omp sections nowait
        {
            //if process not on *** boundary, precondition *** of local domain, otherwise, maintain same BC
            #pragma omp section
            //*** = left
            if( leftRank != MPI_PROC_NULL) {
                for(j = 1; j < Ny-1; ++j) {
                    out[IDX(0,j)] = in[IDX(0,j)]/factor;
                }
            }
            
            #pragma omp section
            if( leftRank == MPI_PROC_NULL) {
                for(j = 1; j < Ny-1; ++j) {
                    out[IDX(0,j)] = in[IDX(0,j)];
                }
            }
            
            //*** = right
            #pragma omp section
            if(rightRank != MPI_PROC_NULL) {
                for(j = 1; j < Ny - 1; ++j) {
                    out[IDX(Nx-1,j)] = in[IDX(Nx-1,j)]/factor;
                }
            }
            
            #pragma omp section
            if(rightRank == MPI_PROC_NULL) {
                for(j = 1; j < Ny - 1; ++j) {
                    out[IDX(Nx-1,j)] = in[IDX(Nx-1,j)];
                }
            }
            
            //*** = bottom  
            #pragma omp section
            if(bottomRank != MPI_PROC_NULL) {   
                for(i = 1; i < Nx - 1; ++i) {
                    out[IDX(i,0)] = in[IDX(i,0)]/factor;
                }
            }

            #pragma omp section
            if(bottomRank == MPI_PROC_NULL) {
                for(i = 1; i < Nx - 1; ++i) {
                    out[IDX(i,0)] = in[IDX(i,0)];
                }
            }
            
            //*** = top
            #pragma omp section
            if(topRank != MPI_PROC_NULL) {   
                for(i = 1; i < Nx - 1; ++i) {
                    out[IDX(i,Ny-1)] = in[IDX(i,Ny-1)]/factor;
                }
            }

            #pragma omp section
            if(topRank == MPI_PROC_NULL) {
                for(i = 1; i < Nx - 1; ++i) {
                    out[IDX(i,Ny-1)] = in[IDX(i,Ny-1)];
                }
            }
        }
    }
    //---------------------------------------------Step 3: Precondition Corners of each Local Domain -----------------------------------------//
    //No parallel here as overheads would be too much for calculating four datapoints
    
    //if process is on the left or bottom, impose BC on bottom left corner 
    if( (leftRank == MPI_PROC_NULL) | (bottomRank == MPI_PROC_NULL)) {
        out[0] = in[0];
    }
    else {//otherwise, precondition
        out[0] = in[0]/factor;
    }
    
    if( (leftRank == MPI_PROC_NULL) | (topRank == MPI_PROC_NULL)) { 
        out[IDX(0,Ny-1)] = in[IDX(0,Ny-1)];
    }
    else{
        out[IDX(0,Ny-1)] = in[IDX(0,Ny-1)]/factor;
    }
    
    if( (rightRank == MPI_PROC_NULL) | (bottomRank == MPI_PROC_NULL)) {
        out[IDX(Nx-1,0)] = in[IDX(Nx-1,0)];
    }
    else{
        out[IDX(Nx-1,0)] = in[IDX(Nx-1,0)]/factor;        
    }    
    
    if((rightRank == MPI_PROC_NULL) | (topRank == MPI_PROC_NULL)) {
        out[IDX(Nx-1,Ny-1)] = in[IDX(Nx-1,Ny-1)];
    }
    else{
        out[IDX(Nx-1,Ny-1)] = in[IDX(Nx-1,Ny-1)]/factor;
    }
}

void SolverCG::ImposeBC(double* inout) {
        
    //only impose BC on relevant boundaries of the boundary processes
    //negligible performance difference between section and for, use for loop as easier
    //at most two statements will ever be executed, so use for construct rather than sections

    #pragma omp parallel private(i,j)
    {
        if(bottomRank == MPI_PROC_NULL) {                               //if bottom process, impose BC on bottom row
            #pragma omp for schedule(dynamic) nowait
                for(i = 0; i < Nx; ++i) {
                    inout[IDX(i,0)] = 0.0;
                }
        }
        
        if(topRank == MPI_PROC_NULL) {
            #pragma omp for schedule(dynamic) nowait
                for(i = 0; i < Nx; ++i) {
                    inout[IDX(i,Ny-1)] = 0.0;                           //BC on top row
                }
        }
        
        if(leftRank == MPI_PROC_NULL) {
            #pragma omp for schedule(dynamic) nowait
                for(j = 0; j < Ny; ++j) {
                    inout[IDX(0,j)] = 0.0;                              //BC on left column
                }
        }
        
        if(rightRank == MPI_PROC_NULL) {
            #pragma omp for schedule(dynamic) nowait
                for(j = 0; j < Ny; ++j) {
                    inout[IDX(Nx-1,j)] = 0.0;                           //BC on right column
                }
        }
    }
}