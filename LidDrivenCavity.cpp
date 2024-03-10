#include <iostream>
#include <iomanip>
#include <fstream>
#include <cstring>
#include <cmath>
using namespace std;

#include <cblas.h>
#include <mpi.h>

/**
 * @brief Macro to map matrix entry i,j onto it's corresponding location in memory, assuming column-wise matrix storage
 * @param I     matrix index i denoting the ith row
 * @param J     matrix index j denoting the jth columns
 */
#define IDX(I,J) ((J)*Nx + (I))                     //define a new operation to improve computation?

#include "LidDrivenCavity.h"
#include "SolverCG.h"

LidDrivenCavity::LidDrivenCavity(MPI_Comm &rowGrid, MPI_Comm &colGrid, int rowRank, int colRank)
{
    comm_row_grid = rowGrid;
    comm_col_grid = colGrid;
    MPIcoords[0] = rowRank;
    MPIcoords[1] = colRank;
    MPI_Comm_rank(comm_row_grid,&size);     //get size of communicator
    
    //int rootCoord[2] = {0,0};                   //root rank
    //int rootRank;
    
    //reduce global values onto all grid only
    MPI_Allreduce(&Nx,&globalNx,1,MPI_INT,MPI_SUM,comm_row_grid);
    MPI_Allreduce(&Lx,&globalLx,1,MPI_DOUBLE,MPI_SUM,comm_row_grid);
    
    MPI_Allreduce(&Ny,&globalNy,1,MPI_INT,MPI_SUM,comm_col_grid);
    MPI_Allreduce(&Ly,&globalLy,1,MPI_DOUBLE,MPI_SUM,comm_col_grid);
    
}

LidDrivenCavity::~LidDrivenCavity()
{
    CleanUp();                                                      //deallocate memory
}

    //getting functions for testing purposes
double LidDrivenCavity::GetDt(){
    return dt;
} 

double LidDrivenCavity::GetT() {
    return T;
}

double LidDrivenCavity::GetDx() {
    return dx;
}   

double LidDrivenCavity::GetDy() {
    return dy;
}   
    
int LidDrivenCavity::GetNx() {
    return globalNx;
}

int LidDrivenCavity::GetNy() {
    return globalNy;
}

int LidDrivenCavity::GetNpts() {
    return globalNx*globalNy;
}

double LidDrivenCavity::GetLx() {
    return globalLx;
}    

double LidDrivenCavity::GetLy() {
    return globalLy;
}    

double LidDrivenCavity::GetRe() {
    return Re;
}

double LidDrivenCavity::GetU() {
    return U;
}

double LidDrivenCavity::GetNu() {
    return nu;
}

void LidDrivenCavity::GetData(double* vOut, double* sOut, double* u0Out, double* u1Out) {
    for(int i = 0; i < Npts; ++i) {
        vOut[i] = v[i];              //copy data for vorticity and streamfunction
        sOut[i] = s[i];
    }
    
    //--------------------For checking velocity, code snippet exactly same as the one used in WriteSolution ---------------//
    for (int i = 1; i < Nx - 1; ++i) {
        for (int j = 1; j < Ny - 1; ++j) {
            u0Out[IDX(i,j)] =  (sOut[IDX(i,j+1)] - sOut[IDX(i,j)]) / dy;     //compute velocity in x direction at every grid point from streamfunction
            u1Out[IDX(i,j)] = -(sOut[IDX(i+1,j)] - sOut[IDX(i,j)]) / dx;     //compute velocity in y direction at every grid point from streamfunction
        }
    }
    for (int i = 0; i < Nx; ++i) {
        u0Out[IDX(i,Ny-1)] = U;                                        //impose x velocity as U at top surface to enforce no-slip boundary condition
    }
    //----------------------------------------------------------------------------------------------------------------------//
}

void LidDrivenCavity::SetDomainSize(double xlen, double ylen)
{
    this->Lx = xlen;
    this->Ly = ylen;
    UpdateDxDy();                                                   //update grid spacing dx dy based off new domain
}

void LidDrivenCavity::SetGridSize(int nx, int ny)
{
    this->Nx = nx;
    this->Ny = ny;
    UpdateDxDy();                                                   //update grid spacing dx dy based off new number of grid points
}

void LidDrivenCavity::SetTimeStep(double deltat)
{
    this->dt = deltat;
}

void LidDrivenCavity::SetFinalTime(double finalt)
{
    this->T = finalt;
}

void LidDrivenCavity::SetReynoldsNumber(double re)
{
    this->Re = re;
    this->nu = 1.0/re;                                              //compute kinematic viscosity from Reynolds number
}

void LidDrivenCavity::Initialise()
{
    CleanUp();                                                      //deallocate memory

    v   = new double[Npts]();                                       //array denoting vorticity, allocated with zero initial condition
    s   = new double[Npts]();                                       //array denoting streamfunction, allocated with zero initial condition
    tmp = new double[Npts]();                                       //temporay array, zeros
    cg  = new SolverCG(Nx, Ny, dx, dy);                             //create solver
}

void LidDrivenCavity::Integrate()
{
    
    int NSteps = ceil(T/dt);                                        //number of time steps required, rounded up
    for (int t = 0; t < NSteps; ++t)
    {
        if((MPIcoords[0] == 0) & (MPIcoords[1] == 0)) {                           //only print on root rank
            std::cout << "Step: " << setw(8) << t
                      << "  Time: " << setw(8) << t*dt
                      << std::endl;                                     //after each step, output time and step information
        }
        Advance();                                                  //solve the spatial problem and the time domain problem for one time step
    }
}

void LidDrivenCavity::WriteSolution(std::string file)
{
    double* u0 = new double[Nx*Ny]();                               //u0 is horizontal x velocity, initialised with zeros
    double* u1 = new double[Nx*Ny]();                               //u1 is vertical y velocity, initialised with zeros
    for (int i = 1; i < Nx - 1; ++i) {
        for (int j = 1; j < Ny - 1; ++j) {
            u0[IDX(i,j)] =  (s[IDX(i,j+1)] - s[IDX(i,j)]) / dy;     //compute velocity in x direction at every grid point from streamfunction
            u1[IDX(i,j)] = -(s[IDX(i+1,j)] - s[IDX(i,j)]) / dx;     //compute velocity in y direction at every grid point from streamfunction
        }
    }
    for (int i = 0; i < Nx; ++i) {
        u0[IDX(i,Ny-1)] = U;                                        //impose x velocity as U at top surface to enforce no-slip boundary condition
    }

    std::ofstream f(file.c_str());                                  //open/create file for output
    std::cout << "Writing file " << file << std::endl;
    int k = 0;
    for (int i = 0; i < Nx; ++i)
    {
        for (int j = 0; j < Ny; ++j)                                //print data in columns (i.e.keep x location constant, and go down y location)
        {
            k = IDX(i, j);                                                  //denotes location of matrix element (i,j) in memory
            f << i * dx << " " << j * dy << " " << v[k] <<  " " << s[k]     //on each line in file, print the grid location (x,y), vorticity...
              << " " << u0[k] << " " << u1[k] << std::endl;                 //streamfunction, x velocity, y velocity at that grid location
        }
        f << std::endl;                                                     //After printing all (y) data for column in grid, proceed to next column...
    }                                                                       //with a space to differentiate between each column
    f.close();                                                      //close file

    delete[] u0;                                                    //deallocate memory
    delete[] u1;
}

void LidDrivenCavity::PrintConfiguration()
{
    if((MPIcoords[0] == 0) & (MPIcoords[1]== 0)) {
        cout << "Grid size: " << globalNx << " x " << globalNy << endl;                         //print the current problem configuration
        cout << "Spacing:   " << dx << " x " << dy << endl;
        cout << "Length:    " << globalLx << " x " << globalLy << endl;
        cout << "Grid pts:  " << globalNx*globalNy << endl;
        cout << "Timestep:  " << dt << endl;
        cout << "Steps:     " << ceil(T/dt) << endl;
        cout << "Reynolds number: " << Re << endl;
        cout << "Linear solver: preconditioned conjugate gradient" << endl;
        cout << endl;
    }
    
    if (nu * dt / dx / dy > 0.25) {                                             //if timestep restriction not satisfied, terminate the program
        if((MPIcoords[0] == 0) & (MPIcoords[1] == 0)) {
            cout << "ERROR: Time-step restriction not satisfied!" << endl;
            cout << "Maximum time-step is " << 0.25 * dx * dy / nu << endl;
        }
        exit(-1);
    }
}

void LidDrivenCavity::CleanUp()
{
    if (v) {                //if array v is not null pointer, then deallocate arrays and solverCG 
        delete[] v;
        delete[] s;
        delete[] tmp;
        delete cg;
    }
}

void LidDrivenCavity::UpdateDxDy()
{
    dx = globalLx / (globalNx-1);       //calculate new spatial steps dx and dy based off current grid numbers (Nx,Ny) and domain size (Lx,Ly)
    dy = globalLy / (globalNy-1);
    
    Npts = Nx * Ny;         //total number of grid points
}

void LidDrivenCavity::Advance()
{
    double dxi  = 1.0/dx;
    double dyi  = 1.0/dy;
    double dx2i = 1.0/dx/dx;
    double dy2i = 1.0/dy/dy;                                                //store 1/dx,1/dy,1/dx/dx,1/dy/dy to optimise performance

    // Boundary node vorticity
    for (int i = 1; i < Nx-1; ++i) {
        // bottom
        v[IDX(i,0)]    = 2.0 * dy2i * (s[IDX(i,0)]    - s[IDX(i,1)]);
        // top
        v[IDX(i,Ny-1)] = 2.0 * dy2i * (s[IDX(i,Ny-1)] - s[IDX(i,Ny-2)])
                       - 2.0 * dyi*U;
    }
    for (int j = 1; j < Ny-1; ++j) {
        // left
        v[IDX(0,j)]    = 2.0 * dx2i * (s[IDX(0,j)]    - s[IDX(1,j)]);
        // right
        v[IDX(Nx-1,j)] = 2.0 * dx2i * (s[IDX(Nx-1,j)] - s[IDX(Nx-2,j)]);
    }

    // Compute interior vorticity
    for (int i = 1; i < Nx - 1; ++i) {
        for (int j = 1; j < Ny - 1; ++j) {
            v[IDX(i,j)] = dx2i*(
                    2.0 * s[IDX(i,j)] - s[IDX(i+1,j)] - s[IDX(i-1,j)])                  //relating to x terms
                        + 1.0/dy/dy*(
                    2.0 * s[IDX(i,j)] - s[IDX(i,j+1)] - s[IDX(i,j-1)]);                 //relating to y terms
        }
    }

    // Time advance vorticity
    for (int i = 1; i < Nx - 1; ++i) {
        for (int j = 1; j < Ny - 1; ++j) {
            v[IDX(i,j)] = v[IDX(i,j)] + dt*(
                ( (s[IDX(i+1,j)] - s[IDX(i-1,j)]) * 0.5 * dxi
                 *(v[IDX(i,j+1)] - v[IDX(i,j-1)]) * 0.5 * dyi)
              - ( (s[IDX(i,j+1)] - s[IDX(i,j-1)]) * 0.5 * dyi
                 *(v[IDX(i+1,j)] - v[IDX(i-1,j)]) * 0.5 * dxi)
              + nu * (v[IDX(i+1,j)] - 2.0 * v[IDX(i,j)] + v[IDX(i-1,j)])*dx2i
              + nu * (v[IDX(i,j+1)] - 2.0 * v[IDX(i,j)] + v[IDX(i,j-1)])*dy2i);
        }
    }
    
    // Solve Poisson problem
    cg->Solve(v, s);
}