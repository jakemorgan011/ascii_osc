#include <stdio.h>
#include <string.h>
#include <stdlib.h>


// could technically be better to just do the tokenizing inside main?

// malloc
typedef struct parsed_buffer{
  char* buffer;
  unsigned int b_size;
  unsigned int* indexing;
  char** token_buffer;
}
