#define _DEFAULT_SOURCE // refer feature_test_macros(7)
#define _GNU_SOURCE

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include <dirent.h> 
#include <errno.h>
#include <string.h>

#include <json-c/json.h>
#include <curl/curl.h>

#define SIG_PER_SECOND_TICK 1
#define SIG_TEN_SECOND_TICK 2

#define true 1
#define false 0
#define PTR void *

typedef unsigned char bool;

typedef struct _stat_struct{
    float mem;
    char *cmdline;
}Stats;

typedef struct _datapoint_struct{
    float cpu; 
    int proc_count;
    Stats **proc_stat;
}Datapoint;

/* Method declarations */
float get_cpu_utilization(void);
void get_total_memory(unsigned long int*);
void get_overall_memory_usage(unsigned long int*, unsigned long int*);
void get_all_proc_memory_usage(Datapoint*);
void dealloc_all_datapoints(void);
void hit_endpoint(const char* );

void create_timer(int, timer_t*);
void start_timer(timer_t, int, int);
int pid_filter (const struct dirent*);
int proc_stat_compare(const void*, const void*);

/* Global variables */
unsigned long int prev_idle_time = 0;
unsigned long int prev_total_time = 0;
 
unsigned long int mem_total;
int datapoint_count = 0;
Datapoint **datapoints = NULL;

int main() {
    timer_t per_second_timer; 
    timer_t ten_second_timer;
    sigset_t sig_set; // a set of signals
    
    /* Initilaize cUrl */
    
    CURLcode res;
    res = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (res != CURLE_OK){
        fprintf(stderr, "curl_global_init() failed: %s\n", curl_easy_strerror(res));
        return 1;
    }

    /* Create the timers */
    create_timer(SIG_PER_SECOND_TICK, &per_second_timer);
    create_timer(SIG_TEN_SECOND_TICK, &ten_second_timer);
    
    /* Create a signal set to block and wait for */
    sigemptyset(&sig_set);
    sigaddset(&sig_set, SIGALRM);
    sigaddset(&sig_set, SIGINT);

    /* Block the signals in the set  */
    sigprocmask(SIG_BLOCK, &sig_set, NULL);
    
    /* Get thet toatal memory */
    get_total_memory(&mem_total);
    printf("Total memory: %luKiB", mem_total);

    /* Reset the time slices */
    get_cpu_utilization();

    /* Number of datapoints is fixed */
    datapoints = malloc(10 * sizeof(PTR));

    /* Start the timers */
    start_timer(per_second_timer, 1, 1);
    start_timer(ten_second_timer, 10, 10);

    bool running = true;
    while (running == true) {
         /* Blocks the calling threading until a signal (in SIG_SET) becomes 
	        pending.
	    */
        unsigned long int mem_free, mem_used;
        siginfo_t sig_info;
        int signal = sigwaitinfo(&sig_set, &sig_info);
        if (signal == SIGINT) {
            dealloc_all_datapoints();
            break;
        }
        if (signal != SIGALRM) continue;
        switch(sig_info._sifields._timer.si_sigval.sival_int){
        case SIG_PER_SECOND_TICK:
        {
            /* Prepare our datapoint */
            Datapoint *datapoint = malloc(sizeof(Datapoint));
            datapoints[datapoint_count++] = datapoint;
            datapoint->proc_stat = NULL; // Needs to be null for realloc to work
            datapoint->proc_count = 0;
            
            /* Get CPU utilization */
            float util = get_cpu_utilization() * 100;            
            printf("CPU Utilization: %.0f%%\n", util);
            datapoint->cpu = util;

            /* Get system memory usage */
            get_overall_memory_usage(&mem_free, &mem_used);
            util = (1.0f * mem_used / mem_total) * 100;
            printf("Memory usage: %lu/%lu KiB (%.0f%%)\n\n", mem_used, 
                   mem_total, util);

            /* Get memory used by each running process */
            get_all_proc_memory_usage(datapoint);

            /* Sort the data */
            qsort(datapoint->proc_stat, datapoint->proc_count, sizeof(PTR), proc_stat_compare);

            /* Go through the list and print the details */
            for (int i = 0; i < datapoint->proc_count; i++){
                Stats* stats = datapoint->proc_stat[i];
                printf("%.0f%% %s\n", stats->mem, stats->cmdline);
            }
        }
            break;
        case SIG_TEN_SECOND_TICK:
        {
            
            /* Seralize our datapoints here */

            struct json_object *root = json_object_new_object();

            struct json_object *name = json_object_new_string("viswa");
            json_object_object_add(root, "name", name);

            struct json_object *data_points = json_object_new_array();
            for (int i = 0; i < datapoint_count; i++){
                Datapoint *datapoint = datapoints[i];
                struct json_object *jdatapoint = json_object_new_object();

                struct json_object *cpu = json_object_new_int((int)datapoint->cpu);
                json_object_object_add(jdatapoint, "cpu_usage", cpu);

                struct json_object *processes = json_object_new_array();
                for (int x = 0; x < 10; x++){
                    Stats *stat = datapoint->proc_stat[x];
                    struct json_object *jproc = json_object_new_object();

                    struct json_object *cmdline = json_object_new_string(stat->cmdline);
                    json_object_object_add(jproc, "name", cmdline);
                    
                    struct json_object *mem = json_object_new_int((int)stat->mem);
                    json_object_object_add(jproc, "ram_usage", mem);

                    json_object_array_add(processes, jproc);
                }

                json_object_object_add(jdatapoint, "processes", processes);
                json_object_array_add(data_points, jdatapoint);
            }   
            json_object_object_add(root, "data_points", data_points);

            //printf("%s", json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN));

            hit_endpoint(json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN));

            json_object_put(root);

            /* Call the endpoint */

            /* Free mem so we don't leak */
            dealloc_all_datapoints();
            break;
        }
        }   
        /* Flush stdout just in case */
        fflush(stdout);
    }

    /* Cleanup */
    free(datapoints);

    timer_delete(per_second_timer);
    timer_delete(ten_second_timer);
    
    curl_global_cleanup();

    return 0;
}

unsigned long long int get_proc_memory_usage(char *pid)
{
    /*  Returns the amount of physical memory used by a process. The second 
       column in the file /proc/{pid}/statm is this value. Refer proc(5)
    */

    FILE *fpstat;
    char strbuff[100] = "/proc/";

    // Prepare our file name
    strcat(strbuff, pid);
    strcat(strbuff, "/statm");

    fpstat = fopen(strbuff, "r");
    if (fpstat == NULL) return -1; 

    // Clear our buffer before reading
    memset(strbuff, 0, sizeof(char) * 100);
    fgets(strbuff, 100, fpstat);    
    fclose(fpstat);

    // We need the second column, we'll loop through and find the first space
    int i = 0;
    while (1){
        if(strncmp(strbuff + i, " ", 1) == 0){
            break;
        }
        i++;
    }
    return strtoll(strbuff + i, NULL, 10);
}

void get_proc_cmd_line(char *pid, char *cmdline)
{
    FILE *fpstat;
    char strbuff[100] = "/proc/";

    // Prepare our file name
    strcat(strbuff, pid);
    strcat(strbuff, "/cmdline");

    fpstat = fopen(strbuff, "r");
    if (fpstat == NULL) return; 

    // Clear our buffer before reading
    memset(strbuff, 0, sizeof(char) * 100);
    fgets(strbuff, 100, fpstat);
    fclose(fpstat);

    memcpy(cmdline, strbuff, strlen(strbuff));
}


void get_all_proc_memory_usage(Datapoint *datapoint)
{
    struct dirent **namelist;

    int count = scandir("/proc/", &namelist, pid_filter, versionsort);
    char cmdline[100] = {0};
    for (int i = 0; i < count; i++){
        char *pid = namelist[i]->d_name;

        memset(cmdline, 0, sizeof(char) * 100);
        get_proc_cmd_line(pid, cmdline);
        // If cmdline is empty, it means the procss is a zombie
        if(strlen(cmdline) == 0) continue; 

        unsigned long long int mem = get_proc_memory_usage(pid);
        
        Stats *stat = (Stats *)malloc(sizeof(Stats));
        stat->mem = (1.0f * mem / mem_total) * 100;
        stat->cmdline = (char *)malloc((sizeof(char) * strlen(cmdline)) + 1);
        strcpy(stat->cmdline, cmdline);

        datapoint->proc_stat = realloc(datapoint->proc_stat, sizeof(PTR) + (sizeof(PTR) * datapoint->proc_count));
        datapoint->proc_stat[datapoint->proc_count] = stat;
        datapoint->proc_count++;
    }

    // Free up allocated memory
    for (int i = 0; i < count; i++){
        struct dirent *item = namelist[i];
        
        free(item);
    }
    free(namelist);
}

void dealloc_all_datapoints(void)
{
    for (int i = 0; i < datapoint_count; i++){
        Stats **proc_stat = datapoints[i]->proc_stat;
        for (int x = 0; x < datapoints[i]->proc_count; x++){
            Stats *stat = proc_stat[x];
            free(stat->cmdline);
            free(stat);
        }
        free(proc_stat);
        free(datapoints[i]);
    }
    datapoint_count=0;
}


float get_cpu_utilization(void)
{
    /* Gets CPU utilization as a float between 0 an 1.
     * *fpstat is a valid FILE pointer for the /proc/stat file.
     * *prev_total_time and *prev_idle_time are the timestamps corresponding to 
     * the last call of CPU utilization. These values are replaced by the values
     * obtained from the latest call to this method.
    */

    FILE *fpstat;
    char line[100];
    char *start;
    unsigned long int time[10];

    fpstat = fopen("/proc/stat", "r");
    if (fpstat == NULL) return 0; 

    fgets(&line[0], 100, fpstat);
    fclose(fpstat); // We don't need the file anymore

    start = &line[4];
    for (int i = 0; i < 10; i++){
        time[i] = strtol(start, &start, 10);
        start++;
    }

    unsigned long int idle_time = time[3];
    unsigned long int total_time = 0;
    for (int i = 0; i < 10; i++){
        total_time += time[i];
    }
    
    unsigned long int del_total = total_time - prev_total_time;
    unsigned long int del_idle = idle_time - prev_idle_time;

    float idle = 1.0f *  del_idle / del_total;
    float util = 1 - idle;

    prev_total_time = total_time;
    prev_idle_time = idle_time;
    
    return util;
}
 
void get_total_memory(unsigned long int *mem_total){
    FILE *fpstat;
    char line[100];
    char *start;

    fpstat = fopen("/proc/meminfo", "r");
    fgets(&line[0], 100, fpstat); // Get the first line
    start = &line[0] + 10; // Discard the "MemInfo:" part of the string
    *mem_total = strtol(start, NULL, 10); // spaces in the beginning are ignored
    fclose(fpstat);
}

void get_overall_memory_usage(unsigned long int *mem_free, unsigned long int *mem_used)
{
    FILE *fpstat;
    char line[100];
    char *start;
    
    /* Regarding the units the meminfo file: it is always KiB. The unit is
     * hardcoded in the source code.
     */
    
    fpstat = fopen("/proc/meminfo", "r");

    fgets(&line[0], 100, fpstat); // Discard the first line

    fgets(&line[0], 100, fpstat); // Get the second line 
    start = &line[0] +  9; // Discard the "MemFree:" part of the string
    *mem_free = strtol(start, NULL, 10);

    fclose(fpstat); // close the stream
    *mem_used = mem_total - *mem_free;
}

void hit_endpoint(const char *json)
{
    CURL *curl;
    CURLcode res;

    curl = curl_easy_init();
    if (curl){
        curl_easy_setopt(curl, CURLOPT_URL, "https://fathomless-thicket-66026.herokuapp.com/viswa/");
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
        curl_easy_setopt(curl,CURLOPT_VERBOSE, 0L);
        
        struct curl_slist *chunk = NULL;
        chunk = curl_slist_append(chunk, "Content-Type: application/json");
        res = curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);

        res = curl_easy_perform(curl);
        if (res != CURLE_OK){
            printf("Failed to hit the endpoint!");
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        }
        curl_slist_free_all(chunk);
        curl_easy_cleanup(curl);
    }

}

void create_timer(int sig_no, timer_t *timer_id)
{
    struct sigevent sig_event;

    sig_event.sigev_signo = SIGALRM;
    sig_event.sigev_notify = SIGEV_SIGNAL;
    sig_event.sigev_value.sival_int = sig_no;

    timer_create(CLOCK_REALTIME, &sig_event, timer_id);
}

void start_timer(timer_t timer_id, int second_interval, int start_after)
{
    struct itimerspec timer_spec;

    timer_spec.it_interval.tv_sec = second_interval;
    timer_spec.it_interval.tv_nsec = 0;
    timer_spec.it_value.tv_sec = start_after;
    timer_spec.it_value.tv_nsec = 1; // start immediatley if start_after = 0

    timer_settime(timer_id, 0, &timer_spec, NULL); // Start ticking
}

int pid_filter (const struct dirent *dir)
{
    if (dir->d_type != DT_DIR) return 0; // Ignore if this is not a directory

    // Check if the directory name is a number (which will be the pid)
    char *end;
    long int val = strtol(dir->d_name, &end, 10); 
    if (val == 0){
        /* If it was a number, the end pointer would be set to point to the
           first non-digit. If it started and ended at the same point, the
           string is not a number 
         */
        if (end == dir->d_name){
            return 0;
        }
    }
    return 1;
}

int proc_stat_compare(const void *l, const void *m){
    /* Sorts the process in desceneding order of memory */
    
    Stats **a = ((Stats**)l);
    Stats **b = ((Stats**)m);
    
    // if l > m then l should come ahead in the list and hence return -1
    if((*a)->mem > (*b)->mem) return -1;
    else if ((*a)->mem < (*b)->mem) return 1;
    else return 0;
}