/*
 * Analysis code for the Brusselator application.
 * Reads variable u_real, u_imag, v_real, v_imag, and computes the norm of U and V.
 * Writes the variables and their computed norm to an ADIOS2 file.
 *
 * Jong Choi
 *
 * @TODO:
 *      - Error checks. What is vector resizing returns an out-of-memory error? Must handle it
 */
#include <iostream>
#include <stdexcept>
#include <cstdint>
#include <cmath>
#include <thread>
#include <cassert>
#include "adios2.h"
#include "decompose_utils.h"

#include <mpi.h>
#include <fftw3-mpi.h>

/*
 * Print info to the user on how to invoke the application
 */
void printUsage()
{
    std::cout
        << "Usage: analysis input_filename output_filename [output_inputdata]\n"
        << "  input_filename:   Name of the input file handle for reading data\n"
        << "  output_filename:  Name of the output file to which data must be written\n"
        << "  output_inputdata: Enter 0 if you want to write the original variables besides the analysis results\n\n";
}

/*
 * MAIN
 */
int main(int argc, char *argv[])
{
    int rank, comm_size, wrank, step_num = 0, err;
    fftw_complex *in;
    fftw_complex *out;
    fftw_plan plan;
    ptrdiff_t alloc_local, alloc_offset, local_n0, local_0_start;
    size_t x_dim, x_off;

    MPI_Init(&argc, &argv);
    fftw_mpi_init();

    MPI_Comm_rank(MPI_COMM_WORLD, &wrank);

    const unsigned int color = 2;
    MPI_Comm comm;
    MPI_Comm_split(MPI_COMM_WORLD, color, wrank, &comm);

    MPI_Comm_rank(comm, &rank);
    MPI_Comm_size(comm, &comm_size);

    if (argc < 3)
    {
        std::cout << "Not enough arguments\n";
        if (rank == 0)
            printUsage();
        MPI_Abort(comm, -1);
    }

    std::string in_filename;
    std::string out_filename;
    bool output_fft_only = true;
    in_filename = argv[1];
    out_filename = argv[2];
    if (argc >= 4)
    {
        std::string out_fft_only_str = argv[3];
        if (out_fft_only_str.compare("0") == 0)
            output_fft_only = false;
    }


    std::size_t u_global_size, v_global_size;
    std::size_t u_local_size, v_local_size;
    
    bool firstStep = true;

    std::vector<std::size_t> shape_u_real;
    std::vector<std::size_t> shape_u_imag;
    std::vector<std::size_t> shape_v_real;
    std::vector<std::size_t> shape_v_imag;
    
    std::vector<double> u_real_data;
    std::vector<double> u_imag_data;
    std::vector<double> v_real_data;
    std::vector<double> v_imag_data;

    std::vector<double> u_fft_real;
    std::vector<double> u_fft_imag;
    std::vector<double> v_fft_real;
    std::vector<double> v_fft_imag;
    
    // adios2 variable declarations
    adios2::Variable<double> var_u_real_in, var_u_imag_in, var_v_real_in, var_v_imag_in;
    adios2::Variable<double> var_u_fft_real, var_u_fft_imag, var_v_fft_real, var_v_fft_imag;
    adios2::Variable<double> var_u_real_out, var_u_imag_out, var_v_real_out, var_v_imag_out;

    // staring offsets and counts
    std::size_t starts_u[3], starts_v[3], counts_u[3], counts_v[3];

    // adios2 io object and engine init
    adios2::ADIOS ad ("adios2_config.xml", comm, adios2::DebugON);

    // IO object and engine for reading
    adios2::IO reader_io = ad.DeclareIO("SimulationOutput");
    adios2::Engine reader_engine = reader_io.Open(in_filename, adios2::Mode::Read, comm);

    // IO object and engine for writing
    adios2::IO writer_io = ad.DeclareIO("AnalysisOutput");
    adios2::Engine writer_engine = writer_io.Open(out_filename, adios2::Mode::Write, comm);

    // read data per timestep
    while(true) {

        // Begin step
        adios2::StepStatus read_status = reader_engine.BeginStep(adios2::StepMode::NextAvailable, 10.0f);
        if (read_status == adios2::StepStatus::NotReady)
        {
            // std::cout << "Stream not ready yet. Waiting...\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            continue;
        }
        else if (read_status != adios2::StepStatus::OK)
        {
            break;
        }
 
        step_num ++;
        if (rank == 0)
            std::cout << "Step: " << step_num << std::endl;

        // Inquire variable and set the selection at the first step only
        // This assumes that the variable dimensions do not change across timesteps

        // Inquire variable
        var_u_real_in = reader_io.InquireVariable<double>("u_real");
        var_u_imag_in = reader_io.InquireVariable<double>("u_imag");
        var_v_real_in = reader_io.InquireVariable<double>("v_real");
        var_v_imag_in = reader_io.InquireVariable<double>("v_imag");

        shape_u_real = var_u_real_in.Shape();
        shape_u_imag = var_u_imag_in.Shape();
        shape_v_real = var_v_real_in.Shape();
        shape_v_imag = var_v_imag_in.Shape();

        // Get the starting offsets and counts for set_selection and define_var calls
        get_starts_counts_3d_decomp (shape_u_real[0], shape_u_real[1], shape_u_real[2], starts_u, counts_u, comm_size, rank);
        get_starts_counts_3d_decomp (shape_v_real[0], shape_v_real[1], shape_v_real[2], starts_v, counts_v, comm_size, rank);

        // Set selection
        var_u_real_in.SetSelection(adios2::Box<adios2::Dims>(
                    {starts_u[0], starts_u[1], starts_u[2]},
                    {counts_u[0], counts_u[1], counts_u[2]}));
        var_u_imag_in.SetSelection(adios2::Box<adios2::Dims>(
                    {starts_u[0], starts_u[1], starts_u[2]},
                    {counts_u[0], counts_u[1], counts_u[2]}));
        var_v_real_in.SetSelection(adios2::Box<adios2::Dims>(
                    {starts_v[0], starts_v[1], starts_v[2]},
                    {counts_v[0], counts_v[1], counts_v[2]}));
        var_v_imag_in.SetSelection(adios2::Box<adios2::Dims>(
                    {starts_v[0], starts_v[1], starts_v[2]},
                    {counts_v[0], counts_v[1], counts_v[2]}));

        // Declare variables to output
        if (firstStep) {
            alloc_local = fftw_mpi_local_size_3d(shape_u_real[0], shape_u_real[1], shape_u_real[2], 
                                                MPI_COMM_WORLD, &local_n0, &local_0_start);

            in = fftw_alloc_complex(alloc_local);
            out = fftw_alloc_complex(alloc_local);

            printf("shape u %ld\n", shape_u_real[0]);

            if (alloc_local != ((shape_u_real[0]*shape_u_real[1]*shape_u_real[2])/comm_size)) {
                fprintf(stderr, "ERROR: fftw local buffer size %lu != local buffer size determined by decomposition."
                                " Exiting.\n", alloc_local);
                //MPI_Abort(MPI_COMM_WORLD, err);
            }

            if ((NULL == in) or (NULL == out)) {
                std::cout << "FATAL ERROR: Could not allocate memory for fftw arrays. Exiting ..";
                MPI_Abort(MPI_COMM_WORLD, err);
            }

            u_fft_real.reserve(alloc_local);
            u_fft_imag.reserve(alloc_local);
            v_fft_real.reserve(alloc_local);
            v_fft_imag.reserve(alloc_local);

            plan = fftw_mpi_plan_dft_3d(shape_u_real[0], shape_u_real[1], shape_u_real[2], 
                                        in, out, MPI_COMM_WORLD, FFTW_FORWARD, FFTW_ESTIMATE);

            x_dim = alloc_local/shape_u_real[1]/shape_u_real[2];
            MPI_Scan(&x_dim, &x_off, 1, MPI_LONG, MPI_SUM, MPI_COMM_WORLD);
            x_off = x_off - x_dim;
            //printf("[%d] alloc_local=%lu, offset=%lu\n", rank, alloc_local, x_off);

            var_u_fft_real = writer_io.DefineVariable<double> ("u_fft_real",
                    { shape_u_real[0]*shape_u_real[1]*shape_u_real[2] },
                    { x_off*shape_u_real[1]*shape_u_real[2] },
                    { (size_t)alloc_local } );
            var_u_fft_imag = writer_io.DefineVariable<double> ("u_fft_imag",
                    { shape_u_real[0]*shape_u_real[1]*shape_u_real[2] },
                    { x_off*shape_u_real[1]*shape_u_real[2] },
                    { (size_t)alloc_local } );

            var_v_fft_real = writer_io.DefineVariable<double> ("v_fft_real",
                    { shape_u_real[0]*shape_u_real[1]*shape_u_real[2] },
                    { x_off*shape_u_real[1]*shape_u_real[2] },
                    { (size_t)alloc_local } );
            var_v_fft_imag = writer_io.DefineVariable<double> ("v_fft_imag",
                    { shape_u_real[0]*shape_u_real[1]*shape_u_real[2] },
                    { x_off*shape_u_real[1]*shape_u_real[2] },
                    { (size_t)alloc_local } );

            if ( !output_fft_only) {
                var_u_real_out = writer_io.DefineVariable<double> ("u_real",
                        { shape_u_real[0], shape_u_real[1], shape_u_real[2] },
                        { x_off, 0, 0 },
                        { x_dim, shape_u_real[1], shape_u_real[2] } );
                var_u_imag_out = writer_io.DefineVariable<double> ("u_imag",
                        { shape_u_real[0], shape_u_real[1], shape_u_real[2] },
                        { x_off, 0, 0 },
                        { x_dim, shape_u_real[1], shape_u_real[2] } );
                var_v_real_out = writer_io.DefineVariable<double> ("v_real",
                        { shape_v_real[0], shape_v_real[1], shape_v_real[2] },
                        { x_off, 0, 0 },
                        { x_dim, shape_u_real[1], shape_u_real[2] } );
                var_v_imag_out = writer_io.DefineVariable<double> ("v_imag",
                        { shape_v_real[0], shape_v_real[1], shape_v_real[2] },
                        { x_off, 0, 0 },
                        { x_dim, shape_u_real[1], shape_u_real[2] } );
            }
            firstStep = false;
        }

        // Set selection
        /*var_u_real_in.SetSelection(adios2::Box<adios2::Dims>(
                    {x_off,0,0},
                    {x_dim, shape_u_real[1], shape_u_real[2]}));
        var_u_imag_in.SetSelection(adios2::Box<adios2::Dims>(
                    {x_off,0,0},
                    {x_dim, shape_u_real[1], shape_u_real[2]}));
        var_v_real_in.SetSelection(adios2::Box<adios2::Dims>(
                    {x_off,0,0},
                    {x_dim, shape_u_real[1], shape_u_real[2]}));
        var_v_imag_in.SetSelection(adios2::Box<adios2::Dims>(
                    {x_off,0,0},
                    {x_dim, shape_u_real[1], shape_u_real[2]}));
        */

        // Read adios2 data
        reader_engine.Get<double>(var_u_real_in, u_real_data);
        reader_engine.Get<double>(var_u_imag_in, u_imag_data);
        reader_engine.Get<double>(var_v_real_in, v_real_data);
        reader_engine.Get<double>(var_v_imag_in, v_imag_data);

        // End adios2 step
        reader_engine.EndStep();

        // FFT for u
        // Input to FFTW
        for (int i = 0; i < x_dim; i++)
        {
            for (int j = 0; j < shape_u_real[1]; j++)
            {
                for (int k = 0; k < shape_u_real[2]; k++)
                {
                    size_t idx = i*shape_u_real[1]*shape_u_real[2] + j*shape_u_real[2] + k;
                    in[idx][0] = u_real_data[idx];
                    in[idx][1] = u_imag_data[idx];
                }
            }
        }

        // Do a fourier transform
        fftw_execute(plan);

        // Output
        for (size_t i = 0; i < alloc_local; i++)
        {
            u_fft_real[i] = out[i][0];
            u_fft_imag[i] = out[i][1];
        }

        // FFT for v
        // Input to FFTW
        for (int i = 0; i < x_dim; i++)
        {
            for (int j = 0; j < shape_u_real[1]; j++)
            {
                for (int k = 0; k < shape_u_real[2]; k++)
                {
                    size_t idx = i*shape_u_real[1]*shape_u_real[2] + j*shape_u_real[2] + k;
                    in[idx][0] = v_real_data[idx];
                    in[idx][1] = v_imag_data[idx];
                }
            }
        }

        // Do a fourier transform
        fftw_execute(plan);

        // Output
        for (size_t i = 0; i < alloc_local; i++)
        {
            v_fft_real[i] = out[i][0];
            v_fft_imag[i] = out[i][1];
        }

        // write U, V, and their norms out
        writer_engine.BeginStep ();
        writer_engine.Put<double> (var_u_fft_real, u_fft_real.data());
        writer_engine.Put<double> (var_u_fft_imag, u_fft_imag.data());
        writer_engine.Put<double> (var_v_fft_real, v_fft_real.data());
        writer_engine.Put<double> (var_v_fft_imag, v_fft_imag.data());
        if (!output_fft_only) {
            writer_engine.Put<double> (var_u_real_out, u_real_data.data());
            writer_engine.Put<double> (var_u_imag_out, u_imag_data.data());
            writer_engine.Put<double> (var_v_real_out, v_real_data.data());
            writer_engine.Put<double> (var_v_imag_out, v_imag_data.data());
        }
        writer_engine.EndStep ();
    }

    // cleanup
    reader_engine.Close();
    writer_engine.Close();

    // Clean up FFTW
    fftw_free(in);
    fftw_free(out);
    fftw_destroy_plan(plan);

    MPI_Finalize();
    return 0;
}

