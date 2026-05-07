#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int is_download_url(const char *url, char *clean_url, size_t clean_len, 
                           char *filename, size_t fn_len) {
    /* Find the # character */
    const char *hash_pos = strchr(url, '#');
    if (!hash_pos) {
        /* No #, not a download URL */
        strncpy(clean_url, url, clean_len - 1);
        clean_url[clean_url - 1] = '\0';
        return 0;
    }
    
    /* Extract everything after # as filename */
    const char *filename_start = hash_pos + 1;
    if (*filename_start == '\0') {
        /* Empty filename, not a download URL */
        strncpy(clean_url, url, clean_len - 1);
        clean_url[clean_len - 1] = '\0';
        return 0;
    }
    
    /* Copy clean URL (everything before #) */
    size_t clean_url_len = hash_pos - url;
    if (clean_url_len >= clean_len) clean_url_len = clean_len - 1;
    strncpy(clean_url, url, clean_url_len);
    clean_url[clean_url_len] = '\0';
    
    /* Copy filename (everything after #) */
    strncpy(filename, filename_start, fn_len - 1);
    filename[fn_len - 1] = '\0';
    
    return 1;
}

int main() {
    char clean_url[512];
    char filename[256];
    
    const char *test_url = "http://site.com/#testinnng";
    
    printf("Original URL: %s\n", test_url);
    
    int is_download = is_download_url(test_url, clean_url, sizeof(clean_url), filename, sizeof(filename));
    
    printf("Is download: %s\n", is_download ? "YES" : "NO");
    printf("Clean URL: %s\n", clean_url);
    printf("Filename: %s\n", filename);
    
    return 0;
}
