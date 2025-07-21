#include "kernel/types.h"
#include "kernel/param.h"
#include "user/user.h"

char*
getline(char *buf, int max)
{
    int i, cc;
    char c;

    for(i=0; i+1 < max; ) {
        cc = read(0, &c, 1);
        if(cc < 1)
            break;
        buf[i++] = c;
        if(c == '\n' || c == '\r') {
            if (i > 0) {
                i--;
            }
            break;
        }
    }
    buf[i] = '\0';
    return buf;
}

void
panic(char *s)
{
  fprintf(2, "%s\n", s);
  exit(1);
}

int
fork1(void)
{
  int pid;

  pid = fork();
  if(pid == -1)
    panic("fork");
  return pid;
}

int
main(int argc, char *argv[])
{   
    char buf[512];
    char *new_argv[MAXARG];
    int i;

    if (argc + 1 > MAXARG + 2) {
        panic("xargs: number of arguments exceed MAXARG");
    }

    if (argc < 2) {
        panic("Usage xargs: program [args]");
    }

    for (i = 0; i < argc - 1;) {
        new_argv[i] = argv[i+1];
        i++;
    }

    while (1) {
        memset(buf, 0, sizeof(buf));
        getline(buf, sizeof(buf));
        if (buf[sizeof(buf)-1] != '\0') {
            panic("xargs: argument length too large");
        }
        if (buf[0] == '\0') {
            break;
        }

        new_argv[i] = buf;
        new_argv[i+1] = 0;
        if (fork1() == 0) {
            exec(argv[1], new_argv);
            panic("xargs: child exec failed");
        } else {
            wait(0);
        }
    } 
    exit(0);
}
