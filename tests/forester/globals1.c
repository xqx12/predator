/*
 * Global variables
 */

#include <stdlib.h>

struct T
{
	struct T* next;
};

struct T* x = NULL;

int main()
{
	if (x)
	{
		// the analyzer should not report a bug here
		*(int*)NULL = 0;
	}

	return 0;
}
