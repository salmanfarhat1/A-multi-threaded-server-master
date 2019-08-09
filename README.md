# A multi-threaded-server
Implementation of a client-server application providing a Twitter-like service, includes register, publish, follow, and history services. It is handled using three types of threads, communicator thread that receives clients' requests, executor thread that executes clients' commands, and answer thread that sends answers back to clients.  The structure of the code contains one communicator thread, and multiple executor and answer threads, with giving some priority to some requests that others. The code implemented in C and tested with extreme configuration. 


### stage 1: 
use multiple client connections, with a communicator thread and an executor thread. 

### stage 2:
Add new type of thread that dedicated to answer to clients

### stage 3: 
use multiple executor threads

### stage 4: 
Process publish actions with high priority
