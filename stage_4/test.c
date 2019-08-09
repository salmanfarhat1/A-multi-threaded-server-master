#include<stdio.h>
#include<unistd.h>
void try(int *f){
	*f = 4;
}
void main(int argc, char *argv[]){
	int x = 3;
	printf("Before : %d " ,x );
	try(&x);
	printf("Before : %d " ,x );
}