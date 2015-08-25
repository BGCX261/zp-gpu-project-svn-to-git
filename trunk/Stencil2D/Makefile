CFLAGS		+= -std=c99 -O3 -I/usr/local/cuda/include -g
LDFLAGS		:= -L/home/acidflow/NVIDIA_GPU_Computing_SDK/ -fopenmp
LDLIBS		:= -lOpenCL 

SOURCES		:= $(wildcard *.cl)
EXEC		:= $(SOURCES:.cl=)

target: $(EXEC)

clean:
	rm -rf $(EXEC) *.o
