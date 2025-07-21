#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
    int i;
    int p;
    int n;
    int *right;
    int p1[2];
    pipe(p1);

    if (fork() == 0) {
        close(p1[1]);

        int left_in = p1[0];
        while (read(left_in, &p, 4) > 0) 
        {   
            fprintf(1, "prime %d\n", p);

            // create right pipe
            int p2[2];
            pipe(p2);
            right = p2;

            if (fork() == 0) {
                close(right[0]);

                while (read(left_in, &n, 4) > 0) 
                {
                    if (n % p != 0) {
                        write(right[1], &n, 4);
                    } 
                }
                close(right[1]);
                close(left_in);
                exit(0);
            } else {
                close(left_in);
                close(right[1]);
                left_in = right[0];
            }
            
        }
        while (wait(0) > 0);
        close(left_in);

    } else {
        close(p1[0]);
        for (i = 2; i <= 35; i++) {
            write(p1[1], &i, 4);
        }
        close(p1[1]);
        wait(0);
    }
    exit(0);
}
