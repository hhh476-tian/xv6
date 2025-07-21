#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"

void 
find(char *dir, char *name)
{
    int fd;
    struct stat st;
    struct dirent de;
    char buf[512], *p;

    if ( (fd = open(dir, 0)) < 0) {
        fprintf(2, "find: cannot find %s\n", dir);
        return;
    }

    if (fstat(fd, &st) < 0) {
        fprintf(2, "find: cannot fstat %s\n", dir);
        return;
    }

    if (st.type != T_DIR) {
        fprintf(2, "find: %s is not a directory\n", dir);
        return;
    }

    while (read(fd, &de, sizeof(de)) == sizeof(de)) {
        if (de.inum == 0) {
            continue;
        }

        if (strcmp(de.name, name) == 0) {
            printf("%s/%s\n", dir, name);
        }

        // skip . and .. directory
        if (strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0) {
            continue;
        }

        // build paths for files in current directory
        if (strlen(dir) + 1 + strlen(de.name + 1) > sizeof(buf)) {
            printf("find: path for subdirectoris too long\n");
        }
        strcpy(buf, dir);
        p = buf + strlen(dir);
        *p = '/';
        p++;
        strcpy(p, de.name);
        p = p + strlen(de.name);
        *p = '\0';

        if(stat(buf, &st) < 0) {
            printf("find: cannot stat %s\n", buf);
            continue;
        }

        if (st.type == T_DIR) {
            find(buf, name);
        }
    }
}

int
main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(2, "Usage: find directory filename\n");
        exit(1);
    }
    find(argv[1], argv[2]);

    exit(0);
}
