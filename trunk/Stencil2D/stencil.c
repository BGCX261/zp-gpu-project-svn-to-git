#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <omp.h>
#include <CL/opencl.h>

#define TIME_DIFF(t1, t2) \
  ((t2.tv_sec - t1.tv_sec) * 1000000 + (t2.tv_usec - t1.tv_usec))

#define MAX_DEVICES 5


#define error(...) do { fprintf(stderr, "Error: " __VA_ARGS__); exit(EXIT_FAILURE); } while(0)
#define check(err, ...)					\
  do {							\
    if(err != CL_SUCCESS) {				\
      fprintf(stderr, "(%d) Error: " __VA_ARGS__, err);	\
      exit(EXIT_FAILURE);				\
    }							\
  } while(0)

size_t file_size(const char *filename) {
	struct stat sb;
	if (stat(filename, &sb) < 0) {
		perror ("stat");
		abort ();
	}
	return sb.st_size;
}

char *
load(const char *filename) {
	FILE *f;
	char *b;
	size_t s;
	size_t r;
	s = file_size (filename);
	b = malloc (s+1);
	if (!b) {
		perror ("malloc");
		exit (1);
	}
	f = fopen (filename, "r");
	if (f == NULL) {
		perror ("fopen");
		exit (1);
	}
	r = fread (b, s, 1, f);
	if (r != 1) {
		perror ("fread");
		exit (1);
	}
	b[s] = '\0';
	return b;
}

#define XDIM      4096
#define YDIM      1024
#define BORDER    1
#define PADDING   ( 64/sizeof(float) - 2*BORDER )
#define LINESIZE  ( XDIM + PADDING + 2*BORDER )
#define OFFSET    (LINESIZE + 16)
#define TOTALSIZE ( LINESIZE*( YDIM + 2*BORDER ) )

#define YDIM_GPU 768
#define YDIM_CPU (YDIM-YDIM_GPU)
#define TOTALSIZE_CPU (LINESIZE*(YDIM_CPU + 2*BORDER))
#define TOTALSIZE_GPU (LINESIZE*(YDIM_GPU + 2*BORDER))
#define OMP 1

/* Version CPU pour comparer le resultat */
void stencil(float* B, const float* A, unsigned int line_size)
{
  for(int y=0; y<YDIM - 1; y++)
    for(int x=0; x<XDIM; x++)
      B[y*LINESIZE + x] = 0.75*A[y*LINESIZE + x] + 
	0.25*( A[y*LINESIZE + x - 1] + A[y*LINESIZE + x + 1] +
	       A[(y-1)*LINESIZE + x] + A[(y+1)*LINESIZE + x]);
}

void stencil_partiel(float* B, const float* A, unsigned int line_size)
{
#pragma omp parallel for
  for(int y=0; y < line_size; y++)
#pragma omp parallel for
    for(int x=0; x<XDIM; x++)
      B[y*LINESIZE + x] = 0.75*A[y*LINESIZE + x] + 
	0.25*( A[y*LINESIZE + x - 1] + A[y*LINESIZE + x + 1] +
	       A[(y-1)*LINESIZE + x] + A[(y+1)*LINESIZE + x]);
}


int main(int argc, char** argv)
{
  cl_platform_id	pf[3];
  cl_uint nb_platforms = 0;
  cl_uint p = 0;

  cl_context context;                 // compute context
  cl_program program;                 // compute program
  cl_int err;                            // error code returned from api calls
    
  cl_device_id devices[MAX_DEVICES];
  cl_uint nb_devices = 0;

  cl_device_type device_type = CL_DEVICE_TYPE_ALL;

  cl_mem d_idata;                       // device memory used for first matrix
  cl_mem d_idata2;                      // device memory used for second matrix
  cl_mem d_odata;                       // device memory used for result matrix
  cl_int dev;
  const unsigned int line_size = LINESIZE;
  const unsigned int mem_size= TOTALSIZE*sizeof(float);
  /* AJOUT DES TAILLE CPU ET GPU */
  const unsigned int mem_size_cpu = TOTALSIZE_CPU*sizeof(float);
  const unsigned int mem_size_gpu = TOTALSIZE_GPU*sizeof(float);

  float *h_idata = NULL;
  float *h_odata = NULL;

  struct timeval tv1,tv2;

  // Filter args
  //
  argv++;
  while (argc > 1) {
    if(!strcmp(*argv, "--gpu-only")) {
      if(device_type != CL_DEVICE_TYPE_ALL)
	error("--gpu-only and --cpu-only can not be specified at the same time\n");
      device_type = CL_DEVICE_TYPE_GPU;
    } else if(!strcmp(*argv, "--cpu-only")) {
      if(device_type != CL_DEVICE_TYPE_ALL)
	error("--gpu-only and --cpu-only can not be specified at the same time\n");
      device_type = CL_DEVICE_TYPE_CPU;
    } else
      break;
    argc--; argv++;
  }

  // Allocation of input & output matrices
  //
  h_idata = malloc(mem_size);
  h_odata = malloc(mem_size);
  float * reference_i = malloc(mem_size);

  // Initialization of input & output matrices
  //
  srand(1234);
  for(unsigned int i = 0; i < TOTALSIZE; i++) {
    h_idata[i]=rand();
    reference_i[i]=h_idata[i];
    h_odata[i]=0.0;
  }

  // Get list of OpenCL platforms detected
  //
  err = clGetPlatformIDs(3, pf, &nb_platforms);
  check(err, "Failed to get platform IDs");

  printf("%d OpenCL platforms detected\n", nb_platforms);

  // Print name & vendor for each platform
  //
  for (unsigned int _p=0; _p<nb_platforms; _p++) {
    cl_uint num;
    int platform_valid = 1;
    char name[1024], vendor[1024];

    err = clGetPlatformInfo(pf[_p], CL_PLATFORM_NAME, 1024, name, NULL);
    check(err, "Failed to get Platform Info");

    err = clGetPlatformInfo(pf[_p], CL_PLATFORM_VENDOR, 1024, vendor, NULL);
    check(err, "Failed to get Platform Info");

    printf("Platform %d: %s - %s\n", _p, name, vendor);

    if(strstr(vendor, "NVIDIA")) {
      p = _p;
      printf("Choosing platform %d\n", p);
    }
  }

  // Get list of devices
  //
  err = clGetDeviceIDs(pf[p], device_type, MAX_DEVICES, devices, &nb_devices);
  printf("nb devices = %d\n", nb_devices);

  // Create compute context with "device_type" devices
  //
  context = clCreateContext (0, nb_devices, devices, NULL, NULL, &err);
  check(err, "Failed to create compute context");

  // Load program source
  const char	*opencl_prog;
  opencl_prog = load("stencil.cl");

  // Build program
  //
  program = clCreateProgramWithSource(context, 1, &opencl_prog, NULL, &err);
  check(err, "Failed to create program");

  err = clBuildProgram (program, 0, NULL, NULL, NULL, NULL);
  check(err, "Failed to build program");

  // Create the input and output buffers in device memory for our calculation
  //
  d_idata = clCreateBuffer(context, CL_MEM_READ_WRITE, mem_size_gpu, NULL, NULL);
  if (!d_idata)
    error("Failed to allocate device memory!\n");

  d_odata = clCreateBuffer(context, CL_MEM_READ_WRITE, mem_size_gpu, NULL, NULL);
  if (!d_odata)
    error("Failed to allocate device memory!\n");

  // Iterate over devices
  //
  for(dev = 0; dev < nb_devices; dev++) {
    cl_command_queue queue;
    cl_kernel kernel;

    char name[1024];
    err = clGetDeviceInfo(devices[dev], CL_DEVICE_NAME, 1024, name, NULL);
    check(err, "Cannot get type of device");

    printf("Device %d : [%s]\n", dev, name);

    // Create a command queue
    //
    queue = clCreateCommandQueue(context, devices[dev], CL_QUEUE_PROFILING_ENABLE, &err);
    check(err,"Failed to create a command queue!\n");

    // Here, we can distinguish between CPU and GPU devices so as
    // to use different kernels, different work group size, etc.
    {
      size_t global[2];                      // global domain size for our calculation
      size_t local[2];                       // local domain size for our calculation

      // Create the compute kernel in the program we wish to run
      //
      kernel = clCreateKernel(program, "stencil", &err);
      check(err, "Failed to create compute kernel!\n");

      // Write our data sets into the device memory
      //
      /* AJOUT ICI */
      err = clEnqueueWriteBuffer(queue, d_idata, CL_TRUE, 0,
				 mem_size_gpu, h_idata + LINESIZE*(YDIM_CPU - 1), 0, NULL, NULL);
      check(err, "Failed to transfer input matrix!\n");

      err = clEnqueueWriteBuffer(queue, d_odata, CL_TRUE, 0,
				 mem_size_gpu, h_odata+LINESIZE*(YDIM_CPU - 1), 0, NULL, NULL);
      check(err, "Failed to transfer input matrix!\n");

      // Set the arguments to our compute kernel
      //
      err = 0;
      err |= clSetKernelArg(kernel, 0, sizeof(cl_mem), &d_odata);
      err |= clSetKernelArg(kernel, 1, sizeof(cl_mem), &d_idata);
      err |= clSetKernelArg(kernel, 2, sizeof(unsigned int), &line_size);
      check(err, "Failed to set kernel arguments! %d\n", err);

      global[0] = XDIM;
      global[1] = YDIM_GPU /4;
      local[0] = 16; // Set workgroup size
      local[1] = 4;

      int numIterations = 200;

      gettimeofday(&tv1, NULL);
      //AJOUT TMP ICI
      float* tmp;
      float* d_omat = h_odata+LINESIZE*(YDIM_CPU - 1);
      float* d_mat = h_idata+LINESIZE*(YDIM_CPU - 1);
      for(int i = 0; i<numIterations; i++) // Iterations are done inside the kernel
      {
	err = clEnqueueNDRangeKernel(queue, kernel, 2, NULL, global, local, 0, NULL, NULL);
	check(err, "Failed to execute kernel!\n");

	// Wait for the command commands to get serviced before reading back results
	//
	clFinish(queue);
	//AJOUT A PARTIR D'ICI
	stencil_partiel(h_odata + OFFSET, h_idata + OFFSET, YDIM_CPU+1);
						// Echange GPU -> CPU
						err = clEnqueueReadBuffer(queue, d_odata, CL_TRUE, LINESIZE*sizeof(float), LINESIZE*sizeof(float),
									d_omat+LINESIZE, 0, NULL, NULL);
						// CPU -> GPU
						err = clEnqueueWriteBuffer(queue, d_odata, CL_TRUE, 0, LINESIZE*sizeof(float),
									d_omat-LINESIZE, 0, NULL, NULL);
					/* NE DOIT PAS MARCHER */	
						tmp = h_odata;
						h_odata = h_idata;
						h_idata = tmp;
						tmp = d_omat;
						d_omat = d_mat;
						d_mat = tmp;

	
      }
      gettimeofday(&tv2, NULL);
      float time1=((float)TIME_DIFF(tv1,tv2)) / 1000;

      // Read back the results from the device to verify the output
      //
      err = clEnqueueReadBuffer(queue, d_odata, CL_TRUE, (LINESIZE*sizeof(float)),
				mem_size_gpu - (LINESIZE*sizeof(float)), d_mat+LINESIZE, 0, NULL, NULL );  
      check(err, "Failed to read output matrix! %d\n", err);

      /* Version cpu pour comparaison */
      float* reference = (float*) malloc(mem_size);
      for(unsigned int i = 0; i < TOTALSIZE; i++)
	reference[i] = 0.0;

      gettimeofday(&tv1,NULL);

      for(int i=0;i<numIterations;++i) {
	stencil(reference + OFFSET, reference_i + OFFSET, line_size);
      }
      gettimeofday(&tv2,NULL);
      float time2=((float)TIME_DIFF(tv1,tv2)) / 1000;

      printf("%f\t%f ms (%fGo/s)\t%f ms (%fGo/s)\n", time2/time1,
	     time1, numIterations * 3*mem_size / time1 / 1000000,
	     time2, numIterations * 3*mem_size / time2 / 1000000);

      // Validate our results
      //
      unsigned int errors=0;
      for(unsigned int i=0;i<TOTALSIZE;i++){
	if((reference[i]-h_idata[i])/reference[i] > 1e-6) {
	  if(errors < 10) printf("%f vs %f\n", h_idata[i], reference[i]);
	  errors++;
	}
      }
      if(errors)
	fprintf(stderr,"%d erreurs !\n", errors);
      else
	fprintf(stderr,"pas d'erreurs, cool !\n");

      clReleaseKernel(kernel);

      free(reference);
    }

      clReleaseCommandQueue(queue);
  }

  // Shutdown and cleanup
  //
  free(h_odata);
  free(h_idata);
  clReleaseMemObject(d_odata);
  clReleaseMemObject(d_idata);
  clReleaseMemObject(d_idata2);
  clReleaseProgram(program);
  clReleaseContext(context);

  return 0;
}

