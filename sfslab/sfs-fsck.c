WeChat: cstutorcs
QQ: 749389476
Email: tutorcs@163.com
/** Filesystem consistency checker for the Shark File System.

    This program checks a SFS disk image (or actual partition,
    I suppose) for the following structural inconsistencies:

      * Mislabeled blocks
      * Invalid directory entries
      * File length disagrees with number of blocks allocated to file
      * Inconsistent doubly linked lists (p->next->prev != p or
        p->prev->next != p, except when p->next or p->prev is NULL)
      * Circular doubly linked lists
      * Blocks that are on more than one list simultaneously
      * Blocks that are not on _any_ list

    Unlike the Unix 'fsck' utility, this program cannot correct any
    problems it encounters.

    You are encouraged to add additional checks to this program.
    However, you should not _need_ to make any changes to it, unless
    you are tackling an optional challenge trace whose comments
    specifically mention that you will need to modify sfs-fsck.c.  */

#include "sfs-disk.h"

#include <argp.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/** Amount of detail printed during the checking process.  */
static unsigned int verbose = 0;

/** The bytemap is a C-string with one byte per block, indicating what
    we know about the disk image at any point in the process of
    checking.  Its primary purpose is to identify blocks that, after
    tracing all the lists, cannot be reached; it also helps us identify
    invalid list structures (e.g. circular lists and blocks that are
    on more than one list).

    These are the byte codes used in the bytemap.  They have been
    organized to allow the bytemap to distinguish the lists for as
    many files as possible.  If you make SFS capable of holding more
    than 250 files, or more than one directory, you will need to
    change how the bytemap works.  */
enum
{
    /** Sentinel: one block past the end of the disk */
    B_end_of_disk = 0x00,
    /** Block has not yet been visited by the checker */
    B_unvisited = 0x01,
    /** Something is wrong with this block */
    B_corrupt = 0x02,
    /** The super block */
    B_super = 0x03,
    /** Block on the free list */
    B_free = 0x04,
    /** Extended root directory block */
    B_rootdir = 0x05,
    /** Block belongs to the first live file we processed.  The second
        live file will be given code B_file0 + 1, the third B_file0 + 2,
        et cetera.  */
    B_file0 = 0x06
};

/** Write the first N chars of char array S (which is *not* considered
    to be a C string) to file FP, converting unprintable characters to
    backslash escapes.  Backslash itself, ", and ' are also escaped.  */
static void fput_escaped_n(const unsigned char *s, size_t n, FILE *fp)
{
    for (size_t i = 0; i < n; i++)
    {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\')
        {
            fputs("\\\\", fp);
        }
        else if (c == '"')
        {
            fputs("\\\"", fp);
        }
        else if (c == '\'')
        {
            fputs("\\'", fp);
        }
        else if (c == '\n')
        {
            fputs("\\n", fp);
        }
        else if (c == '\t')
        {
            fputs("\\n", fp);
        }
        else if (c >= ' ' && c <= '~')
        { // FIXME condition assumes ASCII
            fputc(c, fp);
        }
        else
        {
            fprintf(fp, "\\%02X", c);
        }
    }
}

/** Given an SFS_BLOCK_TYPE_ code, return a human-readable label for it,
    or NULL if the code is not recognized.  */
static const char *sfs_block_type_label(const unsigned char *code)
{
    if (!memcmp(code, SFS_BLOCK_TYPE_FILE, 4))
    {
        return "part of a file";
    }
    else if (!memcmp(code, SFS_BLOCK_TYPE_DIR, 4))
    {
        return "part a directory";
    }
    else if (!memcmp(code, SFS_BLOCK_TYPE_FREE, 4))
    {
        return "unallocated";
    }
    else if (!memcmp(code, SFS_DISK_MAGIC, 4))
    {
        return "the superblock";
    }
    else
    {
        return NULL;
    }
}

/** Given a bytemap code, return a human-readable label for it.
    The string returned by this function may be overwritten by the
    next call to this function.  */
static const char *block_label(unsigned char block_type)
{
    switch (block_type)
    {
    case B_end_of_disk:
        return "[past the end of the disk]";
    case B_unvisited:
        return "[not yet visited]";
    case B_corrupt:
        return "[corrupt block]";
    case B_super:
        return "[super block]";
    case B_free:
        return "free list";
    case B_rootdir:
        return "root directory";
    default:
    {   /* case B_file0...: */
        // 3 chars are sufficient to print any number in [0, 256]
        static char label[sizeof "file " + 3];
        // the cast to unsigned char is mathematically unnecessary but
        // makes it apparent to gcc's printf format checker that
        // 'label' is big enough to hold the formatted string
        snprintf(label, sizeof label, "file %hhu",
                 ((unsigned char)(block_type - B_file0)));
        return label;
    }
    }
}

/** Report that the type of block B was expected to be EXP but has
    been found to be GOT.  Note that both EXP and GOT are likely to
    contain unprintable characters, and GOT is *not* a C string.  */
static void report_bad_block_type(const char *disk, block_id b,
                                  const unsigned char *got,
                                  const unsigned char *exp)
{
    // Friendlier presentation in the common case where 'got' and 'exp'
    // are both standard SFS_BLOCK_TYPE_* codes.
    const char *got_label = sfs_block_type_label(got);
    const char *exp_label = sfs_block_type_label(exp);

    fprintf(stderr, "%s: error: block %u was expected to be ", disk, b);
    if (exp_label)
    {
        fputs(exp_label, stderr);
    }
    else
    {
        fputs("tagged '", stderr);
        fput_escaped_n((const unsigned char *)exp, 4, stderr);
        fputc('\'', stderr);
    }
    if (got_label)
    {
        fprintf(stderr, " but it is instead %s\n", got_label);
    }
    else
    {
        fputs("but it has invalid type tag '", stderr);
        fput_escaped_n(got, 4, stderr);
        fputs("'\n", stderr);
    }
}

/** Map the disk image into RAM.  This is essentially the same code as
    sfs-disk.c:sfs_mount, except it maps the image read-only.  */
static int map_disk_image(const char *image_name,
                          sfs_filesystem_t **superblock_out,
                          size_t *image_size_out)
{
    int fd = open(image_name, O_RDONLY);
    if (fd < 0)
    {
        perror(image_name);
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st))
    {
        perror(image_name);
        return -1;
    }

    // FIXME block devices have st.st_size == 0
    if (st.st_size == 0)
    {
        fprintf(stderr, "%s: error: disk image is empty\n", image_name);
        return 1;
    }
    if (st.st_size > (off_t)SSIZE_MAX || (size_t)st.st_size > SFS_MAX_DISK_SIZE)
    {
        fprintf(
            stderr,
            "%s: error: disk image is too large to hold an SFS file system\n"
            "    (image size: %llu bytes; max supported size: %zu bytes)\n",
            image_name, (unsigned long long)st.st_size, SFS_MAX_DISK_SIZE);
        return -1;
    }
    size_t image_size = (size_t)st.st_size;

    if (verbose)
    {
        fprintf(stderr, "%s: info: size %zu bytes (%zu SFS blocks)\n",
                image_name, image_size, image_size / SFS_BLOCK_SIZE);
    }

    // Since we mmap the disk image, its size must be a multiple of
    // the system page size, even though the format only requires it to
    // be a multiple of SFS_BLOCK_SIZE.
    size_t pagesize = (size_t)sysconf(_SC_PAGESIZE);
    assert(pagesize != (size_t)-1);
    assert(pagesize % SFS_BLOCK_SIZE == 0);
    if (image_size % pagesize != 0)
    {
        fprintf(stderr,
                "%s: error: image size (%zu bytes) is not a multiple"
                " of the system page size (%zu bytes)\n",
                image_name, image_size, pagesize);
        return -1;
    }

    void *mapping = mmap(NULL, image_size, PROT_READ, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED)
    {
        perror(image_name);
        return -1;
    }
    close(fd);
    *superblock_out = mapping;
    *image_size_out = image_size;
    return 0;
}

/** Given a block ID, look up the corresponding block.  */
static const sfs_block_hdr_t *get_block(const sfs_filesystem_t *superblock,
                                        block_id id)
{
    if (id == 0)
        return NULL;
    if (id > superblock->n_blocks)
        abort();
    return (sfs_block_hdr_t *)(((const char *)superblock) +
                               SFS_BLOCK_SIZE * id);
}

/** Validate one list of blocks, whose first block is FIRST_ID.

    Each block in the list must not have previously been visited
    (this indicates two lists pointing to the same block); the list
    must not be circular (this is detected as a side effect of
    detecting two lists pointing to the same block); each block's
    ->next and ->prev pointers must be consistent with its neighbors'
    ->next and ->prev pointers; and the type tag for each block on the
    list must agree with the bytemap tag LIST_TYPE.  If N_BLOCKS_OUT
    is not NULL, and we reach the end of the main loop, the variable
    N_BLOCKS_OUT points to is set to the number of blocks in the list.  */
static int check_blocklist(const char *disk, const sfs_filesystem_t *superblock,
                           unsigned char *bytemap, block_id first_id,
                           unsigned char list_type, uint32_t *n_blocks_out)
{
    if (verbose)
    {
        fprintf(stderr, "%s: info: checking blocklist for %s, first block %u\n",
                disk, block_label(list_type), first_id);
    }

    const unsigned char *expected_block_type;
    if (list_type == B_free)
    {
        expected_block_type = (const unsigned char *)SFS_BLOCK_TYPE_FREE;
    }
    else if (list_type == B_rootdir)
    {
        expected_block_type = (const unsigned char *)SFS_BLOCK_TYPE_DIR;
    }
    else if (list_type >= B_file0)
    {
        expected_block_type = (const unsigned char *)SFS_BLOCK_TYPE_FILE;
    }
    else
    {
        abort();
    }

    block_id cur_id = first_id, prev_id = 0;
    uint32_t n_blocks = 0;
    int status = 0;
    while (cur_id)
    {
        if (cur_id > superblock->n_blocks)
        {
            if (prev_id == 0)
                fprintf(stderr,
                        "%s: error: first block of %s is out of range"
                        " (id %u > %u)",
                        disk, block_label(list_type), cur_id,
                        superblock->n_blocks);
            else
                fprintf(stderr,
                        "%s: error: block %u of %s points to next block"
                        " %u which is out of range (> %u)",
                        disk, prev_id, block_label(list_type), cur_id,
                        superblock->n_blocks);
            return 1;
        }

        if (bytemap[cur_id] == list_type)
        {
            fprintf(stderr,
                    "%s: error: circular links for %s detected at block %u\n",
                    disk, block_label(list_type), cur_id);
            return 1;
        }
        else if (bytemap[cur_id] != B_unvisited)
        {
            // This must be two separate fprintfs so that the result
            // of the first call to block_label is printed before we
            // make the second call to block_label.
            fprintf(stderr, "%s: error: block %u of %s is also part of", disk,
                    cur_id, block_label(list_type));
            fprintf(stderr, " %s\n", block_label(bytemap[cur_id]));
            return 1;
        }

        const sfs_block_hdr_t *cur_blk = get_block(superblock, cur_id);
        if (memcmp(cur_blk->type, expected_block_type, sizeof(cur_blk->type)))
        {
            report_bad_block_type(disk, cur_id, cur_blk->type,
                                  expected_block_type);
            bytemap[cur_id] = B_corrupt;
            // In this case we keep walking the linked list, on the assumption
            // that it's _only_ the block type that's been trashed.
            status = 1;
        }
        else
        {
            bytemap[cur_id] = list_type;
        }

        if (cur_blk->prev_block != prev_id)
        {
            if (prev_id == 0)
            {
                fprintf(stderr,
                        "%s: error: first block of %s (id %u) has prev pointer"
                        " referring to block %u\n",
                        disk, block_label(list_type), cur_id, prev_id);
            }
            else if (cur_blk->prev_block == 0)
            {
                fprintf(stderr,
                        "%s: error: block %u of %s has null prev pointer\n",
                        disk, cur_id, block_label(list_type));
            }
            else
            {
                fprintf(stderr,
                        "%s: error: block %u of %s has prev pointer"
                        " referring to block %u (should be %u)\n",
                        disk, cur_id, block_label(list_type),
                        cur_blk->prev_block, prev_id);
            }
            // In this case we keep walking the forward list, on the assumption
            // that it's _only_ the back pointer that's been trashed.
            status = 1;
        }

        n_blocks++;
        prev_id = cur_id;
        cur_id = cur_blk->next_block;
    }
    if (n_blocks_out)
    {
        *n_blocks_out = n_blocks;
    }
    return status;
}

/** Validate an SFS super block and fabricate an initial bytemap.
    Does *not* validate the directory.  */
static int check_superblock(const char *disk,
                            const sfs_filesystem_t *superblock,
                            size_t image_size, unsigned char **bytemap_out)
{
    if (memcmp(superblock->magic, SFS_DISK_MAGIC, sizeof superblock->magic))
    {
        fprintf(stderr, "Disk image '%s' is not an SFS filesystem\n", disk);
        return -1;
    }
    if ((size_t)superblock->n_blocks * SFS_BLOCK_SIZE != image_size)
    {
        fprintf(stderr,
                "Disk image '%s' is the wrong size:\n"
                "    sb expects %zu blocks, have %zu blocks\n",
                disk, (size_t)superblock->n_blocks,
                image_size / SFS_BLOCK_SIZE);
        return -1;
    }

    unsigned char *bytemap = malloc(superblock->n_blocks + 1);
    if (!bytemap)
    {
        perror("bytemap");
        return -1;
    }
    bytemap[0] = B_super;
    memset(bytemap + 1, B_unvisited, superblock->n_blocks - 1);
    bytemap[superblock->n_blocks] = '\0';

    if (check_blocklist(disk, superblock, bytemap, superblock->freelist, B_free,
                        NULL))
        return -1;
    if (check_blocklist(disk, superblock, bytemap, superblock->next_rootdir,
                        B_rootdir, NULL))
        return -1;

    *bytemap_out = bytemap;
    return 0;
}

/** Validate one block's worth of SFS directory entries.
    You will need to change this function if you decide to change the
    rule for when a directory entry is in use (for example, in order
    to make an empty file not allocate any blocks).  */
static int check_directory_entries(const char *disk,
                                   const sfs_filesystem_t *superblock,
                                   const sfs_dir_entry_t *files,
                                   unsigned char *bytemap,
                                   unsigned char *file_tag_p)
{
    int status = 0;
    unsigned char file_tag = *file_tag_p;

    for (size_t i = 0; i < DIR_ENTRIES_PER_BLOCK; i++)
    {
        if (files[i].first_block == 0)
        {
            if (verbose > 1)
            {
                fprintf(stderr, "%s: info: dir entry %zu not in use\n", disk,
                        i);
            }
            // When the entry is not in use, we allow the size and name fields
            // to be garbage.
            continue;
        }
        // For entries that are in use, the name field should contain
        // a valid C string followed by NULs out to the size of the array...
        int saw_nonnul = 0;
        int saw_nul = 0;
        int name_err = 0;
        for (size_t j = 0; j < SFS_FILE_NAME_SIZE_LIMIT; j++)
        {
            if (files[i].name[j])
            {
                if (saw_nul)
                {
                    fprintf(stderr, "%s: error: dir entry %zu: invalid name '",
                            disk, i);
                    fput_escaped_n((const unsigned char *)files[i].name,
                                   SFS_FILE_NAME_SIZE_LIMIT, stderr);
                    fputs("' (non-NUL after NUL)\n", stderr);
                    name_err = 1;
                    break;
                }
                saw_nonnul = 1;
            }
            else
            {
                saw_nul = 1;
            }
        }
        if (!name_err && !saw_nonnul)
        {
            fprintf(stderr, "%s: error: dir entry %zu: invalid name (all NULs)",
                    disk, i);
            name_err = 1;
        }
        if (!name_err && !saw_nul)
        {
            fprintf(stderr, "%s: error: dir entry %zu: invalid name '", disk,
                    i);
            fput_escaped_n((const unsigned char *)files[i].name,
                           SFS_FILE_NAME_SIZE_LIMIT, stderr);
            fputs("' (missing NUL terminator)\n", stderr);
            name_err = 1;
        }
        if (!name_err && verbose)
        {
            // If we get here, the name is well-formed, but it still might
            // not be fully printable: in the classic Unix tradition, we don't
            // enforce any constraints whatsoever on what bytes may appear in
            // a directory entry.  (If we ever introduced subdirectories
            // we would need to carve out another exception for '/'.)
            fprintf(stderr, "%s: info: dir entry %zu is file '", disk, i);
            fput_escaped_n((const unsigned char *)files[i].name,
                           strlen(files[i].name), stderr);
            fprintf(stderr, "', size %u bytes\n", files[i].size);
        }
        status |= name_err;

        // ... and the size should agree with the number of allocated
        // blocks, assuming the allocation list is valid.
        uint32_t nblocks = 0;
        int list_err =
            check_blocklist(disk, superblock, bytemap, files[i].first_block,
                            file_tag, &nblocks);
        status |= list_err;
        if (!list_err)
        {
            uint32_t exp_nblocks = 1;
            if (files[i].size)
            {
                exp_nblocks =
                    (files[i].size + BLOCK_DATA_SIZE - 1) / BLOCK_DATA_SIZE;
            }
            if (exp_nblocks != nblocks)
            {
                fprintf(stderr,
                        "%s: error: dir entry %zu: size %u requires %u blocks,"
                        " have %u\n",
                        disk, i, files[i].size, exp_nblocks, nblocks);
                status = 1;
            }
        }

        file_tag++;
        if (file_tag == 0)
        {
            fprintf(stderr,
                    "%s: internal error: out of file tags!\n"
                    "    Contact course staff for assistance.\n",
                    disk);
            status = 1;
        }
    }

    *file_tag_p = file_tag;
    return status;
}

/** Validate an SFS root directory and the allocation lists for all
    the files it describes.  This function can handle a root directory
    that occupies more than one block on disk, even though that's left
    as an optional exercise for students.  */
static int check_root_directory(const char *disk,
                                const sfs_filesystem_t *superblock,
                                unsigned char *bytemap)
{
    unsigned char file_tag = B_file0;
    int status;

    if (verbose)
    {
        fprintf(stderr,
                "%s: info: checking root directory entries in superblock\n",
                disk);
    }
    status = check_directory_entries(disk, superblock, superblock->files,
                                     bytemap, &file_tag);

    block_id b = superblock->next_rootdir;
    while (b)
    {
        if (verbose)
        {
            fprintf(stderr,
                    "%s: info: checking root directory entries in block %u\n",
                    disk, b);
        }
        // This list was already run through check_blocklist by
        // check_superblock, so we can safely assume all the block
        // pointers are valid and each block is in fact a directory block.
        const sfs_block_hdr_t *dh = get_block(superblock, b);
        status |= check_directory_entries(disk, superblock,
                                          ((sfs_block_dir_t *)dh)->files,
                                          bytemap, &file_tag);
        b = dh->next_block;
    }
    return status;
}

/** As the final step, check whether there are any blocks that weren't visited
    at all, i.e. they aren't reachable via any of the lists. */
static int check_for_lost_blocks(const char *disk,
                                 const sfs_filesystem_t *superblock,
                                 const unsigned char *bytemap)
{
    int status = 0;
    if (verbose)
    {
        fprintf(stderr, "%s: info: checking for lost blocks\n", disk);
    }

    unsigned char *c;
    for (c = (unsigned char *)strchr((const char *)bytemap, B_unvisited); c;
         c = (unsigned char *)strchr((const char *)(c + 1), B_unvisited))
    {
        block_id i = (block_id)(c - bytemap);
        const sfs_block_hdr_t *h = get_block(superblock, i);
        const char *label = sfs_block_type_label(h->type);
        if (label)
        {
            fprintf(stderr,
                    "%s: error: block %u (%s) is not on any block list\n", disk,
                    i, label);
        }
        else
        {
            fprintf(stderr, "%s: error: block %u (tag '", disk, i);
            fput_escaped_n(h->type, 4, stderr);
            fprintf(stderr, "') is not on any block list\n");
        }
        status = 1;
    }
    return status;
}

// Command line parsing functions and data
static const struct argp_option command_line_options[] = {
    {"verbose", 'v', 0, 0,
     "Describe progress of the file system check (repeat for more detail)", 0},
    {0, 0, 0, 0, 0, 0}};

static int command_line_parse_1(int key, char *arg, struct argp_state *state)
{
    char **diskp = state->input;
    switch (key)
    {
    case 'v':
        verbose += 1;
        if (verbose == 0)
        {
            argp_error(state, "cannot be that verbose");
        }
        return 0;
    case ARGP_KEY_ARG:
        if (*diskp)
        {
            argp_error(state, "can only check one disk image per invocation");
        }
        *diskp = arg;
        return 0;
    case ARGP_KEY_END:
        if (!*diskp)
        {
            argp_error(state, "need a disk image to check");
        }
        return 0;
    default:
        return ARGP_ERR_UNKNOWN;
    }
}

static const struct argp command_line_spec = {
    command_line_options,
    command_line_parse_1,
    "DISK-IMAGE",
    "\nCheck an SFS disk image for structural inconsistencies.\n"
    "\n"
    "Options:",
    NULL,
    NULL,
    NULL};

int main(int argc, char **argv)
{
    char *disk = NULL;
    int err = argp_parse(&command_line_spec, argc, argv, 0, 0, &disk);
    if (err)
    {
        fprintf(stderr, "argp_parse: %s\n", strerror(err));
        return 1;
    }

    sfs_filesystem_t *superblock;
    size_t imagesize;
    if (map_disk_image(disk, &superblock, &imagesize))
        return 1;

    unsigned char *bytemap;
    if (check_superblock(disk, superblock, imagesize, &bytemap))
        return 1;

    int status = check_root_directory(disk, superblock, bytemap);
    status |= check_for_lost_blocks(disk, superblock, bytemap);

    if (status == 0 && verbose)
    {
        fprintf(stderr, "%s: info: no errors found\n", disk);
    }
    return status;
}
