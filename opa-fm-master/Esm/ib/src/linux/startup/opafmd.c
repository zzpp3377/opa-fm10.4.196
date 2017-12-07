#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <ctype.h>
#include <pthread.h>
#include <errno.h>
#include <fm_xml.h>
#include <oib_utils.h>

#define OPAFMD_PIPE "/var/run/opafmd"

#define NO_SUCH_COMPONENT 0
#define SM_COMPONENT 1
#define FE_COMPONENT 2
#define FM_NUM_COMPONENTS 3
#define FM_MAX_INSTANCES 4

#define FM_XML_CONFIG "/etc/opa-fm/opafm.xml"

extern char *optarg;
extern int optind;

struct thread_data {
	int component;
	int instance;
};

struct restartCounter_t {
	struct componentCounter_t {
		time_t lastUpdate;
		unsigned int counter;
	} components[FM_NUM_COMPONENTS];
};

struct daemonConfig_t {
	struct instanceConfig_t {
		int enabled;
		struct componentConfig_t {
			int enabled;
			unsigned int maxRetries;
			unsigned int retryTimeout;
		} component[FM_NUM_COMPONENTS];
	} instance[FM_MAX_INSTANCES];
} config = {
	{				//config.instance[*]
		{
			0,		//config.instance[*].enabled
			{{0}}   //config.instance[*].component[*]
		}
	}
};

char *progName;
char *nullEnv[] = {0};
int smPID[FM_MAX_INSTANCES] = {0};
int fePID[FM_MAX_INSTANCES] = {0};
struct restartCounter_t instanceRestarts[FM_MAX_INSTANCES] = {};
int doStop = 0;
int fd = -1;

const char *componentToString(const unsigned int component);
const char *componentToExecName(const unsigned int component);
int *componentToPIDArray(const unsigned int component);
void reloadSMs(void);
void Usage(int err);
int checkRestarts(const unsigned int instance, const unsigned int component);
void sig_handler(int signo, siginfo_t *siginfo, void *context);
int parseInput(char *buf);
int spawn(const unsigned int instance, const int component);
int pkill(const unsigned int instance, const int component);
void updateInstances(void);
int loadConfig(void);
int daemon_main(void);
void *kill_thread(void *arg);

extern FSTATUS oib_get_hfi_from_portguid(IN uint64_t portGuid,
                                         OUT char *pCaName,
                                         OUT int * caNum,
                                         OUT int * portNum,
                                         OUT uint64_t * pPrefix,
                                         OUT uint16_t * pSMLid,
                                         OUT uint8_t * pSMSL,
										 OUT uint8_t * pPortState);

/**
 * Returns string representation of component.
 * 
 * @param component 
 * 
 * @return const char* 
 */
const char *componentToString(const unsigned int component){
	switch (component){
	case SM_COMPONENT:
		return "SM";
	case FE_COMPONENT:
		return "FE";
	default:
		return "";
	}
}

/**
 * Returns string representation of component's executable.
 * 
 * @param component 
 * 
 * @return const char* 
 */
const char *componentToExecName(const unsigned int component){
	switch (component){
	case SM_COMPONENT:
		return "sm";
	case FE_COMPONENT:
		return "fe";
	default:
		return "";
	}
}

/**
 * Returns associated PID array for component.
 * 
 * @param component 
 * 
 * @return int* 
 */
int *componentToPIDArray(const unsigned int component){
	switch (component){
	case SM_COMPONENT:
		return smPID;
	case FE_COMPONENT:
		return fePID;
	default:
		return NULL;
	}
}

/**
 * Sends SIGHUP to all running SM instances.
 */
void reloadSMs(void){
	int inst = 0;
	for (inst = 0; inst < FM_MAX_INSTANCES; ++inst){
		if (smPID[inst])
			kill(smPID[inst], SIGHUP);
	}
}

/**
 * Print program usage information
 * 
 * @param err Program exit code
 */
void Usage(int err){
	fprintf(stderr, "Usage: %s -D\n", progName);
	fprintf(stderr, "       %s [-i instance] [sm|fe] start|stop|restart|halt\n", progName);
	exit(err);
}

/**
 * Checks restart counter specified to see if the counter has 
 * exceeded the threshold over a the course of it's grace 
 * period. 
 * 
 * @param instance 
 * @param component 
 * 
 * @return int 0 if counter has not surpassed threshold, 1 
 *  	   otherwise
 */
int checkRestarts(const unsigned int instance, const unsigned int component){
	struct componentCounter_t *count = &instanceRestarts[instance].components[component];
	int retryTimeout = config.instance[instance].component[component].retryTimeout;
	int maxRetries = config.instance[instance].component[component].maxRetries;
	time_t now = time(NULL);

	if ((now - count->lastUpdate) > retryTimeout){
		count->counter = 1;
	} else {
		count->counter += 1;
	}
	count->lastUpdate = (1 > 0 ? now : count->lastUpdate);
	return count->counter > maxRetries;
}

/**
 * Handle Linux signals, specifically SIGTERM, SIGHUP and 
 * SIGCHLD. 
 * 
 * @param signo Caught signal
 * @param siginfo Signal information, ignored.
 * @param context Stack frame where signal occured, ignored.
 */
void sig_handler(int signo, siginfo_t *siginfo, void *context) {
	char *val = NULL;
	int ret;

	if(doStop || fd < 0)
		return;
	if(signo == SIGTERM){
		val = "sig_term\n";
	} else if (signo == SIGHUP){
		val = "sig_hup\n";
	} else if (signo == SIGCHLD){
		val = "sig_chld\n";
	}
	if (val) {
		ret = write(fd, val, strlen(val));
		if (ret != strlen(val)) {
			doStop = 1;
		}
	}
	return;
}

/**
 * pthread function to kill an instance of an FM component.
 * 
 * @param arg Pointer to thread_data structure.
 * 
 * @return void* NULL
 */
void *kill_thread(void *arg){
	struct thread_data *data = arg;
	pkill(data->instance, data->component);
	free(data);
	return NULL;
}

/**
 * Function to parse values passed through named pipe to the 
 * daemon. 
 * 
 * @param buf Pointer to the string extracted from the named 
 *  		  pipe
 * 
 * @return int Returns 1 if the daemon recieved a command to 
 *  	   stop.
 */
int parseInput(char *buf){
	char *token;
	int i, c;
	struct thread_data *thread = NULL;
	pthread_t thread_id[FM_MAX_INSTANCES*FM_NUM_COMPONENTS];
	token = strtok(buf, " ");
	if(token == NULL){
		fprintf(stderr, "Received invalid input. Ignoring.\n");
		return 0;
	}
	if(strncmp(token, "sig_chld", 8) == 0){
		int i = 0, pid = 0, stat = 0;
		unsigned int instance = 0, component = 0;
		while ((pid = waitpid(-1, &stat, WNOHANG)) > 0) {
			for(i = 0; i < FM_MAX_INSTANCES; ++i){
				if(smPID[i] == pid){
					component = SM_COMPONENT;
					instance = i;
					break;
				} else if (fePID[i] == pid){
					component = FE_COMPONENT;
					instance = i;
					break;
				}
			}
			if(component == 0){
				continue;
			}
			fprintf(stderr, "%s component of instance %d has terminated with exit code %d\n", componentToString(component), instance, stat);
			if(stat == 0 || stat == 1){ //Skip restarting these components if expected (return 0) or user error (return 1)
				if(stat == 1)
					fprintf(stderr, "Check opafm.xml config for invalid settings.\n");
				return 0;
			}
			if(checkRestarts(instance, component)){
				//Component has failed too many times
				doStop = 3;
				return 0;
			}
			if (pkill(instance, component) == 0){
				spawn(instance, component);
			} else {
				//Failed to kill child. Might be a zombie.
				doStop = 3;
				return 0;
			}
		}
	} else if(!strncmp(token, "stop", 4) || !strncmp(token, "sig_term", 8)){
		token = strtok(NULL, " ");
		if(token == NULL) token = "a";
		switch(token[0]){
		case '0':	// Kill both SM and FE for this instance number.
			doStop = 1;
			token = strtok(NULL, " ");
			if(token == NULL){
				fprintf(stderr, "Received invalid input. Ignoring.\n");
				return 0;
			}
			i = atoi((const char*)token);
			if(i >= FM_MAX_INSTANCES || i < 0){
				fprintf(stderr, "Received invalid input. Ignoring.\n");
				return 0;
			}
			for(c = FM_NUM_COMPONENTS - 1; c >= 1; --c){
				thread = calloc(1, sizeof(struct thread_data));
				if(thread == NULL){
					fprintf(stderr, "Failed to allocate thread struct.\n");
					return 0;
				}
				thread->instance = i;
				thread->component = c;
				pthread_create(&thread_id[c], NULL, &kill_thread, thread);
			}
			for(c = 1; c < FM_NUM_COMPONENTS; ++c){
				pthread_join(thread_id[c], NULL);
			}
			break;
		case '1':	// SM and FE are treated the same so this prevents code redundancy
		case '2':
			c = atoi((const char*)token);
			token = strtok(NULL, " ");
			if(token == NULL){
				fprintf(stderr, "Received invalid input. Ignoring.\n");
				return 0;
			}
			i = atoi((const char*)token);
			pkill(i, c);
			break;
		case 'a':	// Kill all running instances.
			doStop = 1;
			for(i = 0; i < FM_MAX_INSTANCES; ++i){
				for(c = 1; c < FM_NUM_COMPONENTS; ++c){
					thread = calloc(1, sizeof(struct thread_data));
					if(thread == NULL){
						fprintf(stderr, "Failed to allocate thread struct.\n");
						return 0;
					}
					thread->instance = i;
					thread->component = c;
					pthread_create(&thread_id[i*FM_NUM_COMPONENTS+c], NULL, &kill_thread, thread);
				}
			}
			for(i = 0; i < FM_MAX_INSTANCES; ++i){
				for(c = 1; c < FM_NUM_COMPONENTS; ++c){
        	                        pthread_join(thread_id[i*FM_NUM_COMPONENTS+c], NULL);
                	        }
			}
			return 1;
		default:
			fprintf(stderr, "Unknown component value. (%d)\n", token[0]);
		}
		return 0;
	}
	if(strncmp(token, "start", 5) == 0){
		token = strtok(NULL, " ");
		if(token == NULL){
			fprintf(stderr, "Received invalid input. Ignoring.\n");
			return 0;
		}
		switch(token[0]){
		case '0':
			token = strtok(NULL, " ");
			if(token == NULL){
				fprintf(stderr, "Received invalid input. Ignoring.\n");
				return 0;
			}
			i = atoi((const char*)token);
			if(i > FM_MAX_INSTANCES){	//Adding additional instance check here to avoid duplicate error messages
				fprintf(stderr, "Received invalid instance number %d. Ignoring.\n", i);
				return 0;
			}
			spawn(i, SM_COMPONENT);
			spawn(i, FE_COMPONENT);
			break;
		case '1':
		case '2':
			c = atoi((const char*)token);
			token = strtok(NULL, " ");
			if(token == NULL){
				fprintf(stderr, "Received invalid input. Ignoring.\n");
				return 0;
			}
			i = atoi((const char*)token);
			spawn(i, c);
			break;
		default:
			fprintf(stderr, "Unknown component value.\n");
		}
	} else if(strncmp(token, "sig_hup", 7) == 0){
		loadConfig();
		updateInstances();
		reloadSMs();
	} else if(strncmp(token, "restart", 7) == 0){
		token = strtok(NULL, " ");
		if(token == NULL){
			fprintf(stderr, "Received invalid input. Ignoring.\n");
			return 0;
		}
		switch(token[0]){
		case '0':
			token = strtok(NULL, " ");
			if(token == NULL){
				fprintf(stderr, "Received invalid input. Ignoring.\n");
				return 0;
			}
			i = atoi((const char*)token);
                        if(i > FM_MAX_INSTANCES){	//Adding additional instance check here to avoid duplicate error messages
                                fprintf(stderr, "Received invalid instance number %d. Ignoring.\n", i);
                                return 0;
                        }
			if(pkill(i, SM_COMPONENT) == 0)
				spawn(i, SM_COMPONENT);
			if(pkill(i, FE_COMPONENT) == 0)
				spawn(i, FE_COMPONENT);
			break;
		case '1':
		case '2':
			c = atoi((const char*)token);
			token = strtok(NULL, " ");
			if(token == NULL){
				fprintf(stderr, "Received invalid input. Ignoring.\n");
				return 0;
			}
			i = atoi((const char*)token);
			if(pkill(i, c) == 0)
				spawn(i, c);
			break;
		default:
			fprintf(stderr, "Unknown component value.\n");
		}
	}
	return 0;
}

/**
 * Spawns an instance of the FM component.
 * 
 * @param instance Instance number the component belongs to 
 * @param component Integer representation of the component 
 *  				(1 = SM, 2 = FE)
 * @return int PID spawned or -1 if an error has occurred
 */
int spawn(const unsigned int instance, const int component){
	sigset_t mask;
	int *pids;
	int pid;
	char prog[32], name[32];
	if (instance >= FM_MAX_INSTANCES){
		fprintf(stderr, "Invalid instance number.\n");
		return -1;
	}
	if ((pids = componentToPIDArray(component)) == NULL){
		fprintf(stderr, "spawn: Invalid component number %d.\n", component);
		return -1;
	}
	if (pids[instance] != 0 && kill(pids[instance], 0) == 0){
		fprintf(stderr, "Instance %d of %s is already running.\n", instance, componentToString(component));
		return -1;
	}
	if (!config.instance[instance].component[component].enabled){
		fprintf(stderr, "Instance %d of %s is not enabled. Please enable instance in %s\n", instance, componentToString(component), FM_XML_CONFIG);
		return -1;
	}
	switch(pid = fork()){
	case 0:
		sigemptyset(&mask);
		sigaddset(&mask,SIGTERM);
		sigaddset(&mask,SIGCHLD);
		sigaddset(&mask,SIGHUP);
		sigprocmask(SIG_UNBLOCK, &mask, NULL);
		sprintf(prog, "/usr/lib/opa-fm/runtime/%s", componentToExecName(component));
		sprintf(name, "%s_%d", componentToExecName(component), instance);
		execle(prog, prog, "-e", name, NULL, nullEnv);
		break;
	case -1:
		fprintf(stderr, "Failed to start %s for instance %d.\n", componentToString(component), instance);
		break;
	default:
		fprintf(stdout, "Started instance %d of %s.\n", instance, componentToString(component));
		if(pid > 0) pids[instance] = pid;
		break;
	}
	return pid;
}

/**
 * Kills instance of FM component 
 * 
 * @param instance Instance number the component belongs to
 * @param component Integer representation of the component 
 *  				(1 = SM, 2 = FE)
 * @return int Returns 0 if process was properly terminated, 
 *  	   otherwise -1 on error.
 */
int pkill(const unsigned int instance, const int component){
	int *pids;
	int pid;
	int status;
	int ret;
	if(instance >= FM_MAX_INSTANCES){
		fprintf(stderr, "Invalid instance number.\n");
		return -1;
	}
	if((pids = componentToPIDArray(component)) == NULL){
		fprintf(stderr, "pkill: Invalid component number %d.\n", component);
		return -1;
	}
	pid = pids[instance];
	if(!pid) return 0;
	if(kill(pid, 0) == 0){
		kill(pid, SIGTERM);							// Here we send a SIGTERM to the PID specified.
		sleep(component == SM_COMPONENT ? 3 : 1); 				// Wait 3 seconds for SM to die from SIGTERM, only 1 sec for FE
		ret = waitpid((pid_t)pid, &status, WUNTRACED | WNOHANG);		// Reap the children if they're already dead. Because zombies are bad, mmkay.
		if(!ret){								// Checks for insubordinate children
			kill(pid, SIGKILL);						// Order a hit on them
			sleep(component == SM_COMPONENT ? 3 : 1);			// Allow 1-3 seconds for murder
			ret = waitpid((pid_t)pid, &status, WUNTRACED | WNOHANG);	// Reap
			if(!ret){							// Report all survivors
				fprintf(stderr, "Failed to kill %s from instance %d\n", componentToString(component), instance);
				return -1;
			}
		}
		instanceRestarts[instance].components[component].lastUpdate = 0;	// Make sure to reset restart timer to prevent manual stop/restart from counting against component
		fprintf(stdout, "Stopped instance %d of %s.\n", instance, componentToString(component));
	} else {
		fprintf(stderr, "Instance %d of %s not running.\n", instance, componentToString(component));
	}
	pids[instance] = 0;	// PID not running or doesn't exist
	return 0;
}

/**
 * Checks config and starts/stop instances according to config.
 */
void updateInstances(void) {
	int inst, comp;
	for (inst = 0; inst < FM_MAX_INSTANCES; ++inst){
		if (config.instance[inst].enabled){
			for (comp = 1; comp < FM_NUM_COMPONENTS; ++comp){
				if (config.instance[inst].component[comp].enabled){
					if (!(componentToPIDArray(comp))[inst])
						spawn(inst, comp);
				} else {
					if ((componentToPIDArray(comp))[inst])
						pkill(inst, comp);
				}
			}
		} else {
			for (comp = 1; comp < FM_NUM_COMPONENTS; ++comp){
				if ((componentToPIDArray(comp))[inst])
					pkill(inst, comp);
			}
		}
	}
}

/**
 * Load up xml parser and extract values 
 * 
 * @return int 0 on success.
 */
int loadConfig(void){
	FMXmlCompositeConfig_t *xml_config = NULL;
	int i, j, hfi, port;

	char      name[UMAD_CA_NAME_LEN];
	uint64_t guid;
	uint64_t  prefix;
	uint16    sm_lid;
	uint8     sm_sl;
	uint8     port_state;

	xml_config = parseFmConfig(FM_XML_CONFIG, IXML_PARSER_FLAG_NONE, /* instance does not matter for startup */ 0, /* full parse */ 1, /* embedded */ 0);
	if (!xml_config) {
		fprintf(stdout, "Could not open or there was a parse error while reading configuration file ('%s')\n", FM_XML_CONFIG);
		return -1;
	}

	for (i = 0; i < FM_MAX_INSTANCES; i++) {
		if (!xml_config->fm_instance[i]) {
			config.instance[i].enabled = 0;
		} else {
			config.instance[i].enabled = xml_config->fm_instance[i]->fm_config.start;
			config.instance[i].component[SM_COMPONENT].enabled = xml_config->fm_instance[i]->sm_config.start;
			config.instance[i].component[SM_COMPONENT].maxRetries = xml_config->fm_instance[i]->sm_config.startup_retries;
			config.instance[i].component[SM_COMPONENT].retryTimeout = xml_config->fm_instance[i]->sm_config.startup_stable_wait * 60;
			config.instance[i].component[FE_COMPONENT].enabled = xml_config->fm_instance[i]->fe_config.start;
			config.instance[i].component[FE_COMPONENT].maxRetries = xml_config->fm_instance[i]->fe_config.startup_retries;
			config.instance[i].component[FE_COMPONENT].retryTimeout = xml_config->fm_instance[i]->fe_config.startup_stable_wait * 60;
		}
	}

	struct { // Struct created to make the if tree below more readable.
		uint32 hfi;
		uint32 port;
		uint64 guid;
	} selected_device[FM_MAX_INSTANCES] = {{0}};

	for (i = 0; i < FM_MAX_INSTANCES; ++i) {
		// +1 for hfi, as both oib_get_hfi_from_portguid and oib_get_portguid assumes 1 based hfi value,
		// and xml_config->fm_instance[i]->fm_config gives zero based hfi
		selected_device[i].hfi = xml_config->fm_instance[i]->fm_config.hca+1;
		selected_device[i].port = xml_config->fm_instance[i]->fm_config.port;
		selected_device[i].guid = xml_config->fm_instance[i]->fm_config.port_guid;
		if (xml_config->fm_instance[i]->fm_config.start) {
			// To make sure we don't miss mismatched guid and hfi/port configs, we want to fill in missing info
			if (selected_device[i].guid && oib_get_hfi_from_portguid(selected_device[i].guid, name, &hfi, &port, &prefix, &sm_lid, &sm_sl, &port_state) == VSTATUS_OK) {
				selected_device[i].hfi = hfi;
				selected_device[i].port = port;
			} else if (selected_device[i].port &&
					   oib_get_portguid(selected_device[i].hfi, selected_device[i].port, NULL, NULL, &guid, NULL, NULL, NULL, NULL, NULL, NULL, NULL) == VSTATUS_OK) {
				selected_device[i].guid = guid;
			}
			if (!selected_device[i].guid || !selected_device[i].port) {
				fprintf(stderr, "Invalid config detected! Coundn't find device for instance %d!.\n", i);
				releaseXmlConfig(xml_config, /* full */ 1);
				return 1;
			}
		}
	}

	for (i = 0; i < FM_MAX_INSTANCES; ++i) {
		if (xml_config->fm_instance[i]->fm_config.start) { // only compare enabled instances
			for (j = i + 1; j < FM_MAX_INSTANCES; ++j) {
				if (xml_config->fm_instance[j]->fm_config.start && // against other enabled instances
					((selected_device[i].hfi == selected_device[j].hfi &&
					selected_device[i].port == selected_device[j].port) ||
					selected_device[i].guid == selected_device[j].guid)) {
						fprintf(stderr, "Invalid config detected! Same HFI detected for multiple enabled FM instances.\n");
						releaseXmlConfig(xml_config, /* full */ 1);
						return 1;
				}
			}
		}
	}

	releaseXmlConfig(xml_config, /* full */ 1);
	return 0;
}

/**
 * Daemon main loop. Sets up named pipe and captures commands 
 * given through named pipe. 
 *  
 * Can be stopped with "stop" command or with SIGTERM. 
 * 
 * @return int Returns negative integer on error.
 */
int daemon_main(){
	char buf[128] = {0};
	FILE *fp;
	struct sigaction act;
	sigset_t mask;
	int res = 0;

	sigemptyset(&mask);
 	sigaddset(&mask,SIGTERM);
 	sigaddset(&mask,SIGCHLD);
 	sigaddset(&mask,SIGHUP);
 
	setlinebuf(stdout);
 
	bzero(&act, sizeof(act));
 	act.sa_sigaction = sig_handler;
 	act.sa_mask = mask;
 	act.sa_flags = SA_NOCLDSTOP|SA_SIGINFO;
	
	sigaction(SIGTERM, &act, NULL);
	sigaction(SIGCHLD, &act, NULL);
	sigaction(SIGHUP, &act, NULL);

	if((res = loadConfig()) != 0){
		if (res == 1) return res;
		fprintf(stderr, "Failed to load conf, continuing with defaults.\n");
	}
	updateInstances();

	unlink(OPAFMD_PIPE); // cleanup any bad states
	if (mkfifo(OPAFMD_PIPE, 0660) == -1){
		fprintf(stderr, "Failed to create pipe: %s\n", strerror(errno));
		return(2);
	}
	if((fd = open(OPAFMD_PIPE, O_RDWR)) == -1){
		fprintf(stderr, "Failed to open pipe for reading: %s\n", strerror(errno));
		unlink(OPAFMD_PIPE); // cleanup any bad states
		return(2);
        }
	if((fp = fdopen(fd, "r")) == NULL){
		fprintf(stderr, "Failed to open pipe for reading: %s\n", strerror(errno));
		unlink(OPAFMD_PIPE); // cleanup any bad states
		return(2);
	}
	sigprocmask(SIG_BLOCK, &mask, NULL);

	while(!doStop){
		char *s;
 		sigprocmask(SIG_UNBLOCK, &mask, NULL);
 		s = fgets(buf, 127, fp);
 		sigprocmask(SIG_BLOCK, &mask, NULL);
 		if (s != NULL) {
 			parseInput(buf);
 		}
	}
	sigprocmask(SIG_UNBLOCK, &mask, NULL);

	fclose(fp);
	doStop = -(doStop - 1);	// Use doStop value as return value. Allows systemd to show daemon status on failure.
	unlink(OPAFMD_PIPE);			// Destroy pipe
	pthread_exit(&doStop);	// Allows any running background threads to terminate properly.
}

int main(int argc, char* argv[]) {
	char opts[] = "dDi:";
	char c;
	int isDebug = 0, isDaemon = 0, res = 0;
	char *cmd = NULL, *comp = NULL, inst[2] = {0};
	progName = argv[0];
	if(argc < 2)
		Usage(1);
	while((c = getopt(argc, argv, opts)) != -1) {
		switch (c) {
		case 'd':
			isDebug = 1;
			// fall through
		case 'D':
			isDaemon = 1;
			break;
		case 'i':
			memcpy(inst, optarg, 1);
			break;
		case '?':
			if (optopt == 'c')
				fprintf(stderr, "Missing argument for option -%c\n", optopt);
			else if (isprint(optopt))
				fprintf(stderr, "Unknown option -%c\n", optopt);
			Usage(0);
			break;
		}
	}
	if(optind < argc && isDaemon){
		fprintf(stderr, "Cannot start daemon with additional arguments.\n");
		Usage(1);
	}
	while (optind < argc) {
		char *arg = argv[optind++];
		switch(arg[0]){
		case 's':
			switch(arg[1]){
			case 't':
				if(strcmp(arg, "start"))
					cmd = "stop";
				else
					cmd = "start";
				break;
			case 'm':
				comp = "sm";
				break;
			default:
				fprintf(stderr, "Unknown parameter %s\n", arg);
				Usage(1);
			}
			break;
		case 'r':
			if (strcmp(arg, "restart")) {
				fprintf(stderr, "Unknown parameter %s\n", arg);
				Usage(1);
			} else {
				cmd = "restart";
			}
			break;
		case 'f':
			if (strcmp(arg, "fe")) {
				fprintf(stderr, "Unknown parameter %s\n", arg);
				Usage(1);
			} else {
				comp = "fe";
			}
			break;
		case 'h':
			if (strcmp(arg, "halt")){
				fprintf(stderr, "Unknown parameter %s\n", arg);
				Usage(1);
			} else {
				int fd;
				if((fd = open(OPAFMD_PIPE, O_WRONLY|O_NONBLOCK)) == -1){
					// Daemon is already stopped
					exit(0);
				}
				res = write(fd, "stop\n", 5);	// write stop command without parameters to named pipe to kill it softly. With our words.
				if(res <= 0){
					//Something went wrong while writing to pipe.
					fprintf(stderr, "Failed to send stop command to daemon: %s\n", strerror(errno));
					close(fd);
					exit(2);
				}
				close(fd);

				// Wait until the pipe stops existing.
				while (access(OPAFMD_PIPE, F_OK) != -1) {
					usleep(100000);
				}
				exit(0);
			}
		default:
			fprintf(stderr, "Unknown parameter %s\n", arg);
			Usage(1);
		}
	}
	if(isDaemon){
		if (isDebug) {
			// don't fork if debug
			return daemon_main();
		}
		// Here we fork the daemon and if successful...
		switch(fork()){
		case 0:
			// daemon inits it's main loop
			return daemon_main();
		case -1:
			fprintf(stderr, "Failed to fork.\n");
			return 2;
		default:
			return 0;
		}
	} else {
		char prog[] = "/usr/lib/opa-fm/bin/opafmctrl";
		if(inst[0] != 0){
			if(comp != NULL)
				execle(prog, prog, cmd, "-i", inst, comp, NULL, nullEnv);	// Call opafmctl with instance number and component,
			else
				execle(prog, prog, cmd, "-i", inst, NULL, nullEnv);			// only with instance number,
		} else {
			if(comp != NULL)
				execle(prog, prog, cmd, comp, NULL, nullEnv);				// only with component,
			else
				execle(prog, prog, cmd, NULL, nullEnv);						// or without additional arguments.
		}
	}
	return 0;
}
