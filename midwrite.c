#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>


int main(int argc, char *argv[])
{
	if (argc != 3 && argc != 4)
	{
	fprintf(stderr, "%s [filename] [start] (text)\n", argv[0]);
	return 1;
	}

	const char *text = (argc > 3)? argv[3] : NULL;

	FILE *write_file = fopen(argv[1], "r+");
	if (!write_file)
	{
	fprintf(stderr, "%s: unable to open '%s': %s\n", argv[0], argv[1], strerror(errno));
	return 1;
	}

	int position = 0;
	char error;
	if (sscanf(argv[2], "%i%c", &position, &error) != 1)
	{
	fprintf(stderr, "%s: invalid offset '%s'\n", argv[0], argv[2]);
	return 1;
	}

	fseek(write_file, position, (position < 0)? SEEK_END : SEEK_SET);


	if (text) fprintf(write_file, "%s", text);
	else
	{
	char buffer[1024];
	while (fgets(buffer, sizeof buffer, stdin))
	fprintf(write_file, "%s", buffer);
	}

	fflush(write_file);
	return 0;
}
