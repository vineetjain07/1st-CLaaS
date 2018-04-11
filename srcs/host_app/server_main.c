/*
**
** The main application that is needed to communicate with the hardware and the 
** python server.
**
** It accepts socket communications to transmit data and commands.
** The access to the hardware resources is possible through the use of a library
** "kernel.c" which contains all the functions that are needed to utilize the FPGA device.
**
** There is a set of functions that handle the communication through the socket.
**
** Author: Alessandro Comodi, Politecnico di Milano
**
*/

#include <fcntl.h>
#include <cstdio>
#include <cstdlib>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <assert.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <CL/opencl.h>

#include "lodepng.h"
#include "kernel.h"
#include "protocol.h"
#include "utility.h"

#define STR_VALUE(arg) #arg
#define GET_STRING(name) STR_VALUE(name)

#if defined(SDX_PLATFORM) && !defined(TARGET_DEVICE)
#define TARGET_DEVICE GET_STRING(SDX_PLATFORM)
#endif

#ifdef KERNEL
#define KERNEL_NAME GET_STRING(KERNEL)
#endif

#define SOCKET "SOCKET"
#define ACK_DATA "ACK_DATA"
#define ACK_SIZE "ACK_SIZE"
#define PORT 8080

#define CHUNK_SIZE 8192
#define MSG_LENGTH 128

using namespace std;
using namespace lodepng;

/*
** Data structure to handle a array of doubles and its size to have a dynamic behaviour
*/
typedef struct array {
  double * data;
  int data_size;
} dynamic_array;

/* 
** This function is needed to translate the message coming from 
** the node js client into a number to be given in input to the
** switch construct to decode the command
*/
int get_command(char * command);

/* 
** Utility function to print errors
*/
void perror(const char * error);

/*
** Utility function to handle the command decode coming from the socket
** connection with the python webserver
*/
cl_data_types handle_command(int socket, int command, cl_data_types opencl, const char *xclbin, const char *kernel_name, int memory_size);

/*
** Utility function to handle the data coming from the socket and sent to the FPGA device
*/
dynamic_array handle_write_data(int socket);

/*
** Utility function to handle the data coming from the FPGA that has to be sent back to the client
*/
int handle_read_data(int socket, unsigned char data[], int data_size);

/*
** Utility function to handle the data coming from the FPGA that has to be sent back to the client
*/
int handle_read_data(int socket, int data[], int data_size);

cl_data_types handle_get_image(int socket, cl_data_types cl, color_transition_t * color_scheme);

int main(int argc, char const *argv[])
{
  // Socket relative variables
  int server_fd, sock;
  struct sockaddr_un address;
  int opt = 1;
  int addrlen = sizeof(address);

  // OpenCL data type definition
  cl_data_types cl;
  cl.status = 1;

  if (argc != 3) {
    printf("Usage: %s xclbin kernel_name\n", argv[0]);
    return EXIT_FAILURE;
  }

  // Name of the .xclbin binary file and the name of the Kernel passed as arguments
  const char *xclbin = argv[1];
  const char *kernel_name = argv[2];

  /************************
  **                     **
  ** Socket related code **
  **                     **
  ************************/

  // Creating socket file descriptor
  if ((server_fd = socket(AF_UNIX, SOCK_STREAM, 0)) == 0)
    perror("Socket failed");
    
  // Attaching UNIX SOCKET
  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT,
                          &opt, sizeof(opt)))
    perror("setsockopt failed");

  address.sun_family = AF_UNIX;
  unlink(SOCKET);
  strncpy(address.sun_path, SOCKET, sizeof(address.sun_path)-1);
    
  // Binding to the UNIX SOCKET
  if (bind(server_fd, (struct sockaddr *)&address, 
                 sizeof(address))<0)
    perror("Bind failed");

  if (listen(server_fd, 3) < 0)
    perror("Listen failed");

  int data_array[262144];

  for (int i = 0; i < sizeof(data_array) / sizeof(int); i++)
    data_array[i] = 0;

  // Dynamic array on which data coming from the client will be saved
  dynamic_array array_struct;
  char msg[MSG_LENGTH];
  char response[MSG_LENGTH];
  
  int command;
  int err;

  // color scheme
  color_transition_t * color_scheme;
  color_scheme = get_color_scheme();

  while (true) {
    if ((sock = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
      printf("%d\n", sock);
      perror("SOCKET: Accept Failure");
    }

    while(true) {
      if(!(err = recv(sock, msg, sizeof(msg), 0))){
        printf("Error %d: Client disconnected\n", err);
        break;
      }

      // message translation in an integer
      command = get_command(msg);
      
      switch( command ) {
        case WRITE_DATA_N:
          sprintf(response, "INFO: Write Data");
          send(sock, response, strlen(response), MSG_NOSIGNAL);

          // Filling the data structure with data coming from the client with the use of the handle_write_data utility function
          array_struct = handle_write_data(sock);

          // Call the OpenCL utility function to send data to the FPGA
          cl = write_kernel_data(cl, array_struct.data, array_struct.data_size * sizeof array_struct.data);
          
          // Free the resources in the data structure
          free(array_struct.data);
          break;
        case READ_DATA_N:
          sprintf(response, "INFO: Read Data");
          send(sock, response, strlen(response), MSG_NOSIGNAL);
          
          // Read data coming from the Kernel and save them in data_array
          cl = read_kernel_data(cl, data_array, sizeof(data_array));

          // Call the utility function to send data over the socket
          handle_read_data(sock, data_array, sizeof(data_array));
          break;
        case GET_IMAGE_N:
          sprintf(response, "INFO: Get Image");
          send(sock, response, strlen(response), MSG_NOSIGNAL);
          cl = handle_get_image(sock, cl, color_scheme);
          break;
        default:
          cl = handle_command(sock, command, cl, xclbin, kernel_name, COLS * ROWS * sizeof(int));
      }
    }
  }

  return 0;
}

void perror(const char * error) {
  printf("%s\n", error);
  exit(EXIT_FAILURE);
}

cl_data_types handle_command(int socket, int command, cl_data_types cl, const char *xclbin, const char *kernel_name, int memory_size) {
  char response[MSG_LENGTH];

  switch (command) {
    // Initialization of the platform
    case INIT_PLATFORM_N:
      if(!cl.initialized) {
        cl = initialize_platform();
        if (cl.status)
          sprintf(response, "Error: could not initialize platform");
        else
          sprintf(response, "INFO: platform initialized");
      } else
        sprintf(response, "Error: Platform already initialized");
      break;

    // Initialization of the kernel (loads the fpga program)
    case INIT_KERNEL_N:
      if(cl.status){
        sprintf(response, "Error: first initialize platform");
        break;
      }

      if(!cl.initialized) {
        cl = initialize_kernel(cl, xclbin, kernel_name, memory_size);
        if (cl.status)
          sprintf(response, "Error: Could not initialize the kernel");
        else {
          sprintf(response, "INFO: kernel initialized");
          cl.initialized = true;
        }
      }
      break;

    // Releasing all OpenCL links to the fpga
    case CLEAN_KERNEL_N:
      cl = clean_kernel(cl);
      sprintf(response, "INFO: Kernel cleaned");
      break;

    // Start Kernel computation
    case START_KERNEL_N:
      cl = start_kernel(cl);
      sprintf(response, "INFO: Started computation");
      break;
    default:
      sprintf(response, "Command not recognized");
      break;
  }
  
  send(socket, response, strlen(response), MSG_NOSIGNAL);

  return cl;
}

/*
** Paramters
** socket: reference to the socket channel with the webserver
*/
dynamic_array handle_write_data(int socket) {
  // Variable definitions
  int data_size, data_read = 0;
  int to_read = CHUNK_SIZE;
  dynamic_array array_struct;
  double * data_chunks;

  // Receive size of the data that will be transmitted by the webserver
  if(!recv(socket, &data_size, sizeof(data_size), 0))
    printf("Data size receive error: Client disconnected\n");

  // Sending ACK to webserver to synchronize
  send(socket, ACK_SIZE, strlen(ACK_SIZE), MSG_NOSIGNAL);

  // Prepare to receive the data allocating space in the memory
  array_struct.data_size = data_size;
  array_struct.data = (double *) malloc(data_size * sizeof *array_struct.data);
  data_chunks = (double *) malloc(CHUNK_SIZE * sizeof *data_chunks);

  // Loop to receive data in chuncks in order to prevent data loss through the socket
  int j = 0;
  while(data_read < data_size) {
    data_read += CHUNK_SIZE;
    to_read = data_read > data_size ? 
                data_size - (data_read - to_read) : 
                CHUNK_SIZE;

    if(!recv(socket, data_chunks, to_read * sizeof *data_chunks, 0))
      printf("Data receive error: Client disconnected\n");

    // Fill the data array incrementally with the chuncks of data
    for (int i = 0; i < to_read; i++)
      array_struct.data[i+(j*CHUNK_SIZE)] = data_chunks[i];

    j++;
  }

  // Sending ACK to state that data was correctly received
  send(socket, ACK_DATA, strlen(ACK_DATA), MSG_NOSIGNAL);
  return array_struct;
}

/*
** Parameters
** socket: reference to the socket channel with the webserver
** data: array containing data to be sent. For this function the array is made of bytes
** data_size: size of the array that has to be sent
*/
int handle_read_data(int socket, unsigned char data[], int data_size) {
  int result_send;
  char ack[MSG_LENGTH];
  if(!recv(socket, ack, sizeof(ack), 0))
    printf("Ack receive error: Client disconnected\n");

  send(socket, &data_size, sizeof(data_size), MSG_NOSIGNAL);

  if(!recv(socket, ack, sizeof(ack), 0))
    printf("Ack receive error: Client disconnected\n");

  if(result_send = send(socket, data, data_size, MSG_NOSIGNAL) < 0)
    perror("Send data failed");

  return 0;
}

/*
** Parameters
** socket: reference to the socket channel with the webserver
** data: array containing data to be sent. For this function the array is made of integers
** data_size: size of the array that has to be sent
*/
int handle_read_data(int socket, int data[], int data_size) {
  int result_send;
  char ack[MSG_LENGTH];
  if(!recv(socket, ack, sizeof(ack), 0))
    printf("Ack receive error: Client disconnected\n");

  send(socket, &data_size, sizeof(data_size), MSG_NOSIGNAL);

  if(!recv(socket, ack, sizeof(ack), 0))
    printf("Ack receive error: Client disconnected\n");

  if(result_send = send(socket, data, data_size, MSG_NOSIGNAL) < 0)
    perror("Send data failed");

  return 0;
}

/*
** Parameters
** socket: reference to the socket channel with the webserver
** cl: OpenCL datatypes
** color_scheme: color transition scheme in order to create the PNG image given the computation results
*/
cl_data_types handle_get_image(int socket, cl_data_types cl, color_transition_t * color_scheme) {
  dynamic_array array_struct;

  input_struct input;

  unsigned char * png;
  size_t pngsize;

  array_struct = handle_write_data(socket);

  for(int i = 0; i < 4; i++)
    input.coordinates[i] = array_struct.data[i];

  input.width = (long) array_struct.data[4];
  input.height = (long) array_struct.data[5];

  input.max_depth = (long) array_struct.data[6];

  cl = write_kernel_data(cl, &input, sizeof input);
  
  // Free the resources in the data structure
  free(array_struct.data);

  // check timing
  struct timespec start, end;
  uint64_t delta_us;
  
  // getting start time
  clock_gettime(CLOCK_MONOTONIC_RAW, &start);

  cl = start_kernel(cl);

  int * data_array = (int *) malloc(input.width * input.height * sizeof(int));

  cl = read_kernel_data(cl, data_array, input.width * input.height * sizeof(int));

  // getting end time
  clock_gettime(CLOCK_MONOTONIC_RAW, &end);
  delta_us = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_nsec - start.tv_nsec) / 1000;

  printf("Execution time GET_IMAGE: %ld [us]\n", delta_us);

  unsigned char * image = (unsigned char *) malloc(input.width * input.height * 3);
  
  int j = 0;
  unsigned int transition;
  unsigned int group;

  // Building the image pixels data
  for(int i = 0; i < input.width * input.height * 3; i+=3) {
    transition = extract_bits(data_array[j], 6, 0);
    group = extract_bits(data_array[j], 26, 6);

    for(int k = 0; k < 3; k++)
      if(data_array[j] == input.max_depth)
        image[i + k] = 0;
      else
        image[i + k] = color_scheme[group%8].color_transition[transition].color[k];

    j++;
  }

  // Generating the png image
  unsigned error = lodepng_encode24(&png, &pngsize, image, input.width, input.height);
  if(error) perror("Error in generating png");

  // Call the utility function to send data over the socket
  handle_read_data(socket, png, (int)pngsize);

  free(data_array);
  free(image);

  return cl;
}

/*
** This function generates a number corresponding to the command that receives in input as a string
*/
int get_command(char * command) {
  if(!strncmp(command, INIT_PLATFORM, strlen(INIT_PLATFORM)))
    return INIT_PLATFORM_N;
  else if(!strncmp(command, INIT_KERNEL, strlen(INIT_KERNEL)))
    return INIT_KERNEL_N;
  else if(!strncmp(command, START_KERNEL, strlen(START_KERNEL)))
    return START_KERNEL_N;
  else if(!strncmp(command, WRITE_DATA, strlen(WRITE_DATA)))
    return WRITE_DATA_N;
  else if(!strncmp(command, READ_DATA, strlen(READ_DATA)))
    return READ_DATA_N;
  else if(!strncmp(command, CLEAN_KERNEL, strlen(CLEAN_KERNEL)))
    return CLEAN_KERNEL_N;
  else if(!strncmp(command, GET_IMAGE, strlen(GET_IMAGE)))
    return GET_IMAGE_N;
  else
    return -1;
}
