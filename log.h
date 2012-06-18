#define ERROR_LOG(fmt, args...) printf("ERROR: %s (%d), %s: " fmt "\n", __FILE__, __LINE__, __func__, ##args)
#define INFO_LOG(fmt, args...)  printf("INFO:  %s (%d), %s: " fmt "\n", __FILE__, __LINE__, __func__, ##args)
