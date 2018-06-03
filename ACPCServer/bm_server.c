/*
Copyright (C) 2011 by the Computer Poker Research Group, University of Alberta
*/

#include <stdlib.h>
#include <stdio.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <assert.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <time.h>
#include <signal.h>
#include <fcntl.h>
#include "game.h"
#include "net.h"
#include "rng.h"


#define STATUS_CLOSED 0
#define STATUS_UNVALIDATED 1
#define STATUS_OKAY 2

#define BM_DEALER "dealer"
#define BM_LOGDIR "logs"
#define BM_DEALER_WAIT_SECS 5
#define BM_MAX_IOWAIT_SECS 1


typedef struct LLPoolEntry_struct {
  struct LLPoolEntry_struct *next;
  struct LLPoolEntry_struct *prev;
  char data[ 0 ];
} LLPoolEntry;

typedef struct {
  LLPoolEntry *head;
  LLPoolEntry *free;
  int dataSize;
  int numEntries;
} LLPool;

/* structure giving the specification for a local bot */
typedef struct {
  char *name;
  char *command;
} BotSpec;

/* structure giving the specification for a user */
typedef struct {
  char *name;
  char *passwd;
  struct timeval waitStart;
} UserSpec;

typedef struct {
  uint16_t maxMatchRuns; /* maximum number of runs for a match */
  uint16_t maxRunningJobs; /* maximum simultaneous jobs at a time for game
			      0 disables the check */
  uint32_t matchHands; /* number of hands in a match */
  Game *game;
  char *gameFile;
  LLPool *bots;

  int curRunningJobs;
} GameConfig;

typedef struct {
  uint16_t port;
  uint16_t maxRunningBots; /* maximum simultaneous bots at a time
			      0 disables the check */
  uint16_t startupTimeoutSecs; /* maximum time to wait for clients to connect
				  0 disables the timer */
  uint16_t responseTimeoutSecs; /* maximum time to wait for clients to respond
                                   with an action */
  uint16_t handTimeoutSecs; /* maximum time to allowed per hand of play */
  uint16_t avgHandTimeSecs; /* average time per hand allowed for the match */

  LLPool *games;
  LLPool *users;
} Config;

typedef struct {
  int status;
  UserSpec *user; /* NULL when status is STATUS_UNVALIDATED */
  ReadBuf *connBuf;
} Connection;

typedef struct {
  GameConfig *gameConf;
  UserSpec *user;
  int numRuns;
  rng_state_t rng;
  uint32_t rngSeed;
  int useRngForSeed; /* 0: use rngSeed as seed for each dealer run
			1: use genrand_int32( match->rng ) */
  char *tag;
  struct timeval queueTime;
  struct {
    int isNetworkPlayer;
    LLPoolEntry *entry; /* connection if network player, bot otherwise */
  } players[ MAX_PLAYERS ];
  int isRunning;
} Match;

typedef struct {
  pid_t dealerPID;
  pid_t botPID[ MAX_PLAYERS ];
  LLPoolEntry *matchEntry;
  char *tag; /* based on tag from the match for this job */
  uint16_t ports[ MAX_PLAYERS ];
} MatchJob;

typedef struct {
  int listenSocket;
  LLPool *conns;
  LLPool *matches;
  LLPool *jobs;

  rng_state_t rng;

  char *hostname;

  int devnullfd;
} ServerState;


LLPool *newLLPool( const int dataSize )
{
  LLPool *pool;

  pool = (LLPool*)malloc( sizeof( LLPool ) );
  assert( pool != 0 );
  pool->head = NULL;
  pool->free = NULL;
  pool->dataSize = dataSize;
  pool->numEntries = 0;
  return pool;
}

int entryInList( LLPoolEntry *list, LLPoolEntry *entry )
{
  while( list ) {

    if( entry == list ) {

      return 1;
    }
    if( list->next ) {

      assert( list->next->prev == list );
    }
    list = list->next;
  }
  return 0;
}

/* add an object to the pool.  data must have a size of pool->dataSize */
LLPoolEntry *LLPoolAddItem( LLPool *pool, void *item )
{
  LLPoolEntry *entry;

  if( pool->free ) {

    entry = pool->free;
    pool->free = entry->next;
  } else {

    entry = (LLPoolEntry*)malloc( sizeof( LLPoolEntry ) + pool->dataSize );
    assert( entry != 0 );
  }

assert( !entryInList( pool->head, entry ) );
  entry->next = pool->head;
  entry->prev = NULL;
  memcpy( entry->data, item, pool->dataSize );
  if( pool->head ) {

    pool->head->prev = entry;
  }
  pool->head = entry;

  ++pool->numEntries;

  return entry;
}

/* remove an item from the pool, placing it in the free list.
   entry must have been generated by LLPoolAddItem( pool, ... )
   (that is, calling LLPoolRemoveEntry on an entry from another pool
   is potentially a very bad idea...) */
void LLPoolRemoveEntry( LLPool *pool, LLPoolEntry *entry )
{
  if( entry->prev ) {

assert( entry->prev->next == entry );
    entry->prev->next = entry->next;
  } else {

assert( pool->head == entry );
    pool->head = entry->next;
  }
  if( entry->next ) {

assert( entry->next->prev == entry );
    entry->next->prev = entry->prev;
  }

assert( !entryInList( pool->free, entry ) );
  if( pool->free ) {

    pool->free->prev = entry;
  }
  entry->next = pool->free;
  pool->free = entry;

  --pool->numEntries;
}

/* LLPool iterator start */
LLPoolEntry *LLPoolFirstEntry( LLPool *pool )
{
  return pool->head;
}

/* removing entries while iterating through the list is fine, as
   long as cur is not the entry being removed. */
LLPoolEntry *LLPoolNextEntry( LLPoolEntry *cur )
{
  if( cur ) {

    return cur->next;
  }
  return NULL;
}

void *LLPoolGetItem( LLPoolEntry *entry )
{
  return &entry->data;
}


void printUsage( FILE *file )
{
  fprintf( file, "usage: bm_server config_file\n" );
}

void setGameDefaults( GameConfig *gameConf )
{
  gameConf->maxMatchRuns = 10;
  gameConf->maxRunningJobs = 1;
  gameConf->matchHands = 5000;
  gameConf->game = NULL;
  gameConf->gameFile = NULL;
  gameConf->bots = newLLPool( sizeof( BotSpec ) );

  gameConf->curRunningJobs = 0;
}

void setDefaults( Config *conf )
{
  conf->port = 54000;
  conf->maxRunningBots = 0;
  conf->startupTimeoutSecs = 600;
  conf->responseTimeoutSecs = 6000; /* Value from 2011 ACPC */
  conf->handTimeoutSecs = 3000 * 7; /* Not enforced for 2011 ACPC */
  conf->avgHandTimeSecs = 70; /* Value from 2011 ACPC */
  conf->games = newLLPool( sizeof( GameConfig ) );
  conf->users = newLLPool( sizeof( UserSpec ) );
}

/* returns entry for bot on success, NULL on failure */
LLPoolEntry *findBot( const GameConfig *game, const char *name )
{
  LLPoolEntry *cur;

  for( cur = LLPoolFirstEntry( game->bots );
       cur != NULL; cur = LLPoolNextEntry( cur ) ) {

    if( !strcmp( ( (BotSpec *)LLPoolGetItem( cur ) )->name, name ) ) {

      return cur;
    }
  }

  return NULL;
}

void addBot( GameConfig *gameConf, const char *spec )
{
  BotSpec bot;
  char name[ READBUF_LEN ];
  char command[ READBUF_LEN ];

  /* split the line into name and command */
  if( sscanf( spec, " %s %s", name, command ) < 2 ) {

    fprintf( stderr, "BM_ERROR: could not get bot name and command from: %s",
	     spec );
    exit( EXIT_FAILURE );
  }

  /* make sure there are no duplicates */
  if( !strcmp( name, "LOCAL" ) ) {

    fprintf( stderr, "BM_ERROR: LOCAL is a reserved bot name\n" );
    exit( EXIT_FAILURE );
  }
  if( findBot( gameConf, name ) ) {

    fprintf( stderr, "BM_ERROR: duplicate bot %s\n", name );
    exit( EXIT_FAILURE );
  }

  /* add the bot */
  bot.name = strdup( name );
  bot.command = strdup( command );
  LLPoolAddItem( gameConf->bots, &bot );
}

/* returns entry for user on success, NULL on failure */
LLPoolEntry *findUser( const Config *conf, const char *name )
{
  LLPoolEntry *cur;

  for( cur = LLPoolFirstEntry( conf->users );
       cur != NULL; cur = LLPoolNextEntry( cur ) ) {

    if( !strcmp( ( (UserSpec *)LLPoolGetItem( cur ) )->name, name ) ) {

      return cur;
    }
  }

  return NULL;
}

void addUser( Config *conf, const char *spec )
{
  UserSpec user;
  char name[ READBUF_LEN ];
  char passwd[ READBUF_LEN ];

  /* split the line into name and password */
  if( sscanf( spec, " %s %s", name, passwd ) < 2 ) {

    fprintf( stderr, "BM_ERROR: could not get user name and password from: %s",
	     spec );
    exit( EXIT_FAILURE );
  }

  /* make sure there are no duplicates */
  if( findUser( conf, name ) ) {

    fprintf( stderr, "BM_ERROR: duplicate user %s\n", name );
    exit( EXIT_FAILURE );
  }

  /* add the user */
  user.name = strdup( name );
  user.passwd = strdup( passwd );
  gettimeofday( &user.waitStart, NULL );
  LLPoolAddItem( conf->users, &user );
}

/* returns entry for game on success, NULL on failure */
LLPoolEntry *findGame( const Config *conf, const char *name )
{
  LLPoolEntry *cur;

  for( cur = LLPoolFirstEntry( conf->games );
       cur != NULL; cur = LLPoolNextEntry( cur ) ) {

    if( !strcmp( ( (GameConfig *)LLPoolGetItem( cur ) )->gameFile, name ) ) {

      return cur;
    }
  }

  return NULL;
}

/* validate a logon request
   returns user on success, or NULL on failure */
UserSpec *validateLogon( const Config *conf, const char *line )
{
  LLPoolEntry *cur;
  char name[ READBUF_LEN ];
  char passwd[ READBUF_LEN ];

  if( sscanf( line, " %s %s", name, passwd ) < 2 ) {

    return NULL;
  }

  for( cur = LLPoolFirstEntry( conf->users );
       cur != NULL; cur = LLPoolNextEntry( cur ) ) {
    UserSpec *user = (UserSpec *)LLPoolGetItem( cur );

    if( !strcmp( user->name, name ) ) {

      if( !strcmp( user->passwd, passwd ) ) {

	return user;
      }
      return NULL;
    }
  }

  return NULL;
}

void readConfig( const char *filename, Config *conf )
{
  int start;
  FILE *file;
  GameConfig *gameConf;
  char *line, lineBuf[ READBUF_LEN ];

  file = fopen( filename, "r" );
  if( file == NULL ) {

    fprintf( stderr, "BM_ERROR: could not open configuration file %s\n",
	     filename );
    exit( EXIT_FAILURE );
  }

  gameConf = NULL;
  while( fgets( lineBuf, READBUF_LEN, file ) ) {

    /* skip past white space at start of line */
    start = 0; while( isspace( lineBuf[ start ] ) ) { ++start; }
    line = &lineBuf[ start ];

    /* ignore comments or empty lines */
    if( line[ 0 ] == '#' || line[ 0 ] == ';'
	|| line[ 0 ] == '\n' || line[ 0 ] == 0 ) {
      continue;
    }

    if( strncasecmp( line, "port", 4 ) == 0 ) {

      if( gameConf != NULL ) {

	fprintf( stderr, "BM_ERROR: server port must be defined outside of game blocks\n" );
	exit( EXIT_FAILURE );
      }
      if( sscanf( &line[ 4 ], "%"SCNu16, &conf->port ) < 1 ) {

	fprintf( stderr, "BM_ERROR: could not get port from: %s", line );
	exit( EXIT_FAILURE );
      }
    } else if( strncasecmp( line, "game", 4 ) == 0 ) {
      FILE *file;
      GameConfig gc;
      char game[ READBUF_LEN ];

      if( gameConf != NULL ) {

	fprintf( stderr, "BM_ERROR: can't define a game within another game block\n" );
	exit( EXIT_FAILURE );
      }
      if( sscanf( &line[ 4 ], " %s", game ) < 1 ) {

	fprintf( stderr, "BM_ERROR: could not get game name from: %s", line );
	exit( EXIT_FAILURE );
      }
      if( findGame( conf, game ) ) {

	fprintf( stderr, "BM_ERROR: game %s has already been used\n", game );
	exit( EXIT_FAILURE );
      }

      setGameDefaults( &gc );
      gc.gameFile = strdup( game );

      file = fopen( gc.gameFile, "r" );
      if( file == NULL ) {

	fprintf( stderr, "BM_ERROR: could not open game file %s\n", gc.gameFile );
	exit( EXIT_FAILURE );
      }
      gc.game = readGame( file );
      fclose( file );

      if( gc.game == NULL ) {

	fprintf( stderr, "BM_ERROR: could not read game %s", gc.gameFile );
	exit( EXIT_FAILURE );
      }
      gameConf
	= (GameConfig *)LLPoolGetItem( LLPoolAddItem( conf->games, &gc ) );
    } else if( strncmp( line, "}", 1 ) == 0 ) {
      /* finished game definition */

      gameConf = NULL;
    } else if( strncasecmp( line, "maxRunningBots", 14 ) == 0 ) {

      if( gameConf != NULL ) {

	fprintf( stderr, "BM_ERROR: maxRunningBots must be defined outside of game blocks\n" );
	exit( EXIT_FAILURE );
      }
      if( sscanf( &line[ 14 ], "%"SCNu16, &conf->maxRunningBots ) < 1 ) {

	fprintf( stderr, "BM_ERROR: could not get maximum number of bots running from: %s", line );
	exit( EXIT_FAILURE );
      }
    } else if( strncasecmp( line, "startupTimeoutSecs", 18 ) == 0 ) {

      if( gameConf != NULL ) {

	fprintf( stderr, "BM_ERROR: startupTimeoutSecs must be defined outside of game blocks\n" );
	exit( EXIT_FAILURE );
      }
      if( sscanf( &line[ 18 ], "%"SCNu16, &conf->startupTimeoutSecs ) < 1 ) {

	fprintf( stderr, "BM_ERROR: could not get maximum dealer startup timeout: %s", line );
	exit( EXIT_FAILURE );
      }
    } else if( strncasecmp( line, "responseTimeoutSecs", 19 ) == 0 ) {

      if( gameConf != NULL ) {

	fprintf( stderr, "BM_ERROR: responseTimeoutSecs must be defined outside of game blocks\n" );
	exit( EXIT_FAILURE );
      }
      if( sscanf( &line[ 19 ], "%"SCNu16, &conf->responseTimeoutSecs ) < 1 ) {

	fprintf( stderr, "BM_ERROR: could not get maximum dealer action timeout: %s", line );
	exit( EXIT_FAILURE );
      }
    } else if( strncasecmp( line, "handTimeoutSecs", 15 ) == 0 ) {

      if( gameConf != NULL ) {

	fprintf( stderr, "BM_ERROR: handTimeoutSecs must be defined outside of game blocks\n" );
	exit( EXIT_FAILURE );
      }
      if( sscanf( &line[ 15 ], "%"SCNu16, &conf->handTimeoutSecs ) < 1 ) {

	fprintf( stderr, "BM_ERROR: could not get maximum dealer hand timeout: %s", line );
	exit( EXIT_FAILURE );
      }
    } else if( strncasecmp( line, "avgHandTimeSecs", 15 ) == 0 ) {

      if( gameConf != NULL ) {

	fprintf( stderr, "BM_ERROR: avgHandTimeSecs must be defined outside of game blocks\n" );
	exit( EXIT_FAILURE );
      }
      if( sscanf( &line[ 15 ], "%"SCNu16, &conf->avgHandTimeSecs ) < 1 ) {

	fprintf( stderr, "BM_ERROR: could not get dealer average hand time: %s", line );
	exit( EXIT_FAILURE );
      }
    } else if( strncasecmp( line, "maxMatchRuns", 12 ) == 0 ) {

      if( gameConf == NULL ) {

	fprintf( stderr, "BM_ERROR: maxMatchRuns must be defined within a game block\n" );
	exit( EXIT_FAILURE );
      }
      if( sscanf( &line[ 12 ], "%"SCNu16, &gameConf->maxMatchRuns ) < 1 ) {

	fprintf( stderr, "BM_ERROR: could not get maximum number of runs in a match from: %s", line );
	exit( EXIT_FAILURE );
      }
    } else if( strncasecmp( line, "maxRunningJobs", 14 ) == 0 ) {

      if( gameConf == NULL ) {

	fprintf( stderr, "BM_ERROR: maxRunningJobs must be defined within a game block\n" );
	exit( EXIT_FAILURE );
      }
      if( sscanf( &line[ 14 ], "%"SCNu16, &gameConf->maxRunningJobs ) < 1 ) {

	fprintf( stderr, "BM_ERROR: could not get maximum number of running jobs from: %s", line );
	exit( EXIT_FAILURE );
      }
    } else if( strncasecmp( line, "matchHands", 10 ) == 0 ) {

      if( gameConf == NULL ) {

	fprintf( stderr, "BM_ERROR: matchHands must be defined within a game block\n" );
	exit( EXIT_FAILURE );
      }
      if( sscanf( &line[ 10 ], "%"SCNu32, &gameConf->matchHands ) < 1 ) {

	fprintf( stderr, "BM_ERROR: could not get number of hands in a match from: %s", line );
	exit( EXIT_FAILURE );
      }
    } else if( strncasecmp( line, "bot", 3 ) == 0 ) {

      if( gameConf == NULL ) {

	fprintf( stderr, "BM_ERROR: matchHands must be defined within a game block\n" );
	exit( EXIT_FAILURE );
      }
      addBot( gameConf, &line[ 3 ] );
    } else if( strncasecmp( line, "user", 4 ) == 0 ) {

      if( gameConf != NULL ) {

	fprintf( stderr,
		 "BM_ERROR: users must be defined outside of game blocks\n" );
	exit( EXIT_FAILURE );
      }
      addUser( conf, &line[ 4 ] );
    } else {

      fprintf( stderr, "BM_ERROR: unknown configuration option %s", line );
      exit( EXIT_FAILURE );
    }
  }

  fclose( file );
}

void addConnection( ServerState *serv, const int sock )
{
  Connection conn;

  /* add the connection */
  conn.status = STATUS_UNVALIDATED;
  conn.user = NULL;
  conn.connBuf = createReadBuf( sock );
  if( conn.connBuf == 0 ) {

    fprintf( stderr, "BM_ERROR: could not create read buffer for socket\n" );
    exit( EXIT_FAILURE );
  }
  LLPoolAddItem( serv->conns, &conn );
}

int matchUsesConnection( const Match *match, const LLPoolEntry *connEntry )
{
  int p;

  for( p = 0; p < match->gameConf->game->numPlayers; ++p ) {

    if( match->players[ p ].isNetworkPlayer
	&& match->players[ p ].entry == connEntry ) {

      return 1;
    }
  }

  return 0;
}

void closeConnection( ServerState *serv, LLPoolEntry *connEntry )
{
  Connection *conn = (Connection*)LLPoolGetItem( connEntry );
  LLPoolEntry *cur, *next;

  destroyReadBuf( conn->connBuf );
  conn->status = STATUS_CLOSED;

  /* remove any pending matches which relied on the connection */
  for( cur = LLPoolFirstEntry( serv->matches ); cur != NULL; cur = next ) {
    next = LLPoolNextEntry( cur );
    Match *match = (Match *)LLPoolGetItem( cur );

    if( matchUsesConnection( match, connEntry ) ) {

      match->numRuns = 0;
    }
  }
}

void handleListenSocket( const Config *conf, ServerState *serv )
{
  int sock;
  struct sockaddr_in addr;
  socklen_t addrLen;

  addrLen = sizeof( addr );
  sock = accept( serv->listenSocket, (struct sockaddr *)&addr, &addrLen );
  if( sock < 0 ) {

    fprintf( stderr, "WARNING: failed to accept incoming connection\n" );
    return;
  }

  addConnection( serv, sock );
}

/* -1 on failure, 0 on success */
int parseMatchSpec( const Config *conf,
		    ServerState *serv,
		    const char *spec,
		    LLPoolEntry *connEntry,
		    Match *match )
{
  uint32_t rngSeed;
  int pos, t, p;
  LLPoolEntry *entry;
  char tag[ READBUF_LEN ];
  char name[ READBUF_LEN ];

  pos = 0;

  if( sscanf( &spec[ pos ], " %s%n", name, &t ) < 1 ) {

    return -1;
  }
  pos += t;

  entry = findGame( conf, name );
  if( entry == NULL ) {

    return -1;
  }
  match->gameConf = (GameConfig *)LLPoolGetItem( entry );

  if( sscanf( &spec[ pos ],
	      " %d %s %"SCNu32" %n",
	      &match->numRuns,
	      tag,
	      &rngSeed,
	      &t ) < 3 ) {

    return -1;
  }
  pos += t;
  if( match->numRuns < 0 || match->numRuns > match->gameConf->maxMatchRuns ) {

    return -1;
  }

  /* make sure tag has no characters in it */
  if( strchr( tag, '/' ) != NULL ) {

    return -1;
  }

  /* get bots */
  for( p = 0; p < match->gameConf->game->numPlayers; ++p ) {

    /* get the name */
    if( sscanf( &spec[ pos ], " %s%n", name, &t ) < 1 ) {

      return -1;
    }
    pos += t;

    /* translate the name into an index */
    if( !strcmp( name, "LOCAL" ) ) {

      match->players[ p ].isNetworkPlayer = 1;
      match->players[ p ].entry = connEntry;
    } else {

      match->players[ p ].isNetworkPlayer = 0;
      match->players[ p ].entry = findBot( match->gameConf, name );
      if( match->players[ p ].entry == NULL ) {

	return -1;
      }
    }
  }

  match->tag = strdup( tag );
  match->rngSeed = rngSeed;
  if( rngSeed ) {

    init_genrand( &match->rng, rngSeed );
    if( match->numRuns == 1 ) {

      match->useRngForSeed = 0;
    } else {

      match->useRngForSeed = 1;
    }
  } else {

    init_genrand( &match->rng, genrand_int32( &serv->rng ) );
  }

  return 0;
}

void writeHelpMessage( int fd )
{
  int r;

  r = write( fd, "HELP - this message\n", 20 );
  r = write( fd, "GAMES - list available games and players\n", 41 );
  r = write( fd, "QSTAT - show the current queue\n", 31 );
  r = write( fd, "RUNMATCHES game #runs tag rngSeed player ... - submit match request\n", 68 );
  r = write( fd, "  - Player order decides match seating\n", 39 );
  r = write( fd, "  - \"LOCAL\" player runs the bm_widget agent (bot_command)\n", 60 );
}

void writeGameList( const Config *conf, int fd )
{
  int r;
  LLPoolEntry *cur, *botCur;
  char line[ READBUF_LEN ];

  for( cur = LLPoolFirstEntry( conf->games );
       cur != NULL; cur = LLPoolNextEntry( cur ) ) {
    GameConfig *game = (GameConfig *)LLPoolGetItem( cur );

    r = snprintf( line, sizeof( line ), "\n%s\n", game->gameFile );
    assert( r > 0 );
    r = write( fd, line, r );

    for( botCur = LLPoolFirstEntry( game->bots );
	 botCur != NULL; botCur = LLPoolNextEntry( botCur ) ) {
      BotSpec *bot = (BotSpec *)LLPoolGetItem( botCur );

      r = snprintf( line, sizeof( line ), " %s\n", bot->name );
      assert( r > 0 );
      r = write( fd, line, r );
    }
  }
}

void writeQueueStatus( const Config *conf, const ServerState *serv, int fd )
{
  int r;
  LLPoolEntry *cur;
  char line[ READBUF_LEN * 4 ];

  if( serv->matches->numEntries == 0 ) {
    r = write( fd, "Queue empty\n", 12 );
  }

  for( cur = LLPoolFirstEntry( serv->matches );
       cur != NULL; cur = LLPoolNextEntry( cur ) ) {
    Match *match = (Match *)LLPoolGetItem( cur );

    r = snprintf( line,
		  sizeof( line ),
		  "%s %s %s * %d %s\n",
		  match->user->name,
		  match->tag,
		  match->gameConf->gameFile,
		  match->numRuns,
		  match->isRunning ? "R" : "Q" );
    assert( r > 0 );
    r = write( fd, line, r );
  }
}

void handleConnection( Config *conf, ServerState *serv,
		       LLPoolEntry *connEntry )
{
  int r;
  Connection *conn = (Connection *)LLPoolGetItem( connEntry );
  char line[ READBUF_LEN ];

  while( ( r = getLine( conn->connBuf, READBUF_LEN, line, 0 ) ) >= 0 ) {

    if( r == 0 ) {

      closeConnection( serv, connEntry );
      return;
    }

    if( conn->status == STATUS_UNVALIDATED ) {
      UserSpec *user;

      user = validateLogon( conf, line );
      if( user == NULL ) {
	/* couldn't authenticate */

	r = write( conn->connBuf->fd, "BAD LOGON\n", 10 );
	fprintf( stderr, "BM_ERROR: connection failed to log in\n" );
	closeConnection( serv, connEntry );
	return;
      }

      /* send an okay message */
      r = write( conn->connBuf->fd, "LOGON OKAY - type help for commands\n", 36 );

      /* connection status is now okay */
      conn->user = user;
      conn->status = STATUS_OKAY;
      return;
    }

    if( !strncasecmp( line, "HELP", 4 ) ) {

      writeHelpMessage( conn->connBuf->fd );
    } else if( !strncasecmp( line, "GAMES", 5 ) ) {

      writeGameList( conf, conn->connBuf->fd );
    } else if( !strncasecmp( line, "QSTAT", 5 ) ) {

      writeQueueStatus( conf, serv, conn->connBuf->fd );
    } else if( !strncasecmp( line, "RUNMATCHES", 10 ) ) {
      Match match;

      if( parseMatchSpec( conf, serv, &line[ 10 ], connEntry, &match ) < 0 ) {

	fprintf( stderr, "BM_ERROR: bad RUNMATCHES command: %s", line );
	r = write( conn->connBuf->fd, "BAD RUNMATCHES COMMAND\n", 23 );
	return;
      }
      match.user = ( (Connection *)LLPoolGetItem( connEntry ) )->user;
      match.isRunning = 0;
      gettimeofday( &match.queueTime, NULL );
      LLPoolAddItem( serv->matches, &match );
      return;
    } else {

      r = write( conn->connBuf->fd, "UNKNOWN\n", 8 );
      return;
    }
  }
}

int timeIsEarlier( struct timeval *a, struct timeval *b )
{
  if( a->tv_sec < b->tv_sec ) {
    return 1;
  } else if( a->tv_sec == b->tv_sec
	     && a->tv_usec < b->tv_usec ) {
    return 1;
  }
  return 0;
}

/* how many bots will match start? */
int botsInMatch( const Match *match )
{
  int p, num;

  num = 0;
  for( p = 0; p < match->gameConf->game->numPlayers; ++p ) {

    if( !match->players[ p ].isNetworkPlayer ) {

      ++num;
    }
  }

  return num;
}

void startDealer( const Config *conf,
		  const Match *match,
		  MatchJob *job,
		  const uint32_t rngSeed )
{
  int stdoutPipe[ 2 ], p, arg;
  char handsString[ 16 ], rngString[ 16 ];
  char startupTimeoutString[ 16 ], responseTimeoutString[ 16 ];
  char handTimeoutString[ 16 ], avgHandTimeString[ 16 ];
  char *argv[ MAX_PLAYERS + 64 ];

  if( pipe( stdoutPipe ) < 0 ) {

    fprintf( stderr, "BM_ERROR: could not create pipe for new dealer\n" );
    exit( EXIT_FAILURE );
  }

  job->dealerPID = fork();
  if( job->dealerPID < 0 ) {

    fprintf( stderr, "BM_ERROR: fork() failed\n" );
    exit( EXIT_FAILURE );
  }
  if( !job->dealerPID ) {
    /* child runs the dealer command */
    int stderrfd;
    char tag[ READBUF_LEN ];

    snprintf( tag, sizeof( tag ), "%s/%s.stderr", BM_LOGDIR, job->tag );
    stderrfd = open( tag, O_WRONLY | O_APPEND | O_CREAT, 0644 );
    if( stderrfd < 0 ) {

      fprintf( stderr,
	       "BM_ERROR: could not create error log %s\n",
	       tag );
      exit( EXIT_FAILURE );
    }
    dup2( stderrfd, 2 );

    /* change stdout to be the write end of the pipe */
    close( stdoutPipe[ 0 ] );
    dup2( stdoutPipe[ 1 ], 1 );

    arg = 0;

    argv[ arg ] = BM_DEALER;
    ++arg;

    snprintf( tag, sizeof( tag ), "%s/%s", BM_LOGDIR, job->tag );
    argv[ arg ] = tag;
    ++arg;

    argv[ arg ] = match->gameConf->gameFile;
    ++arg;

    snprintf( handsString,
	      sizeof( handsString ),
	      "%"PRIu32,
	      match->gameConf->matchHands );
    argv[ arg ] = handsString;
    ++arg;

    snprintf( rngString, sizeof( rngString ), "%"PRIu32, rngSeed );
    argv[ arg ] = rngString;
    ++arg;

    for( p = 0; p < match->gameConf->game->numPlayers; ++p ) {

      if( match->players[ p ].isNetworkPlayer ) {

	argv[ arg ]
	  = ( (Connection *)LLPoolGetItem( match->players[ p ].entry ) )
	  ->user->name;
      } else {

	argv[ arg ]
	  = ( (BotSpec *)LLPoolGetItem( match->players[ p ].entry ) )->name;
      }
      ++arg;
    }

    if( conf->startupTimeoutSecs ) {

      argv[ arg ] = "--start_timeout";
      ++arg;

      snprintf( startupTimeoutString,
                sizeof( startupTimeoutString ),
                "%d",
                (int)conf->startupTimeoutSecs * 1000 );
      argv[ arg ] = startupTimeoutString;
      ++arg;
    }

    /* Add maximum per action timeout argument */
    argv[ arg ] = "--t_response";
    ++arg;

    snprintf( responseTimeoutString,
             sizeof( responseTimeoutString ),
             "%d",
             (int)conf->responseTimeoutSecs * 1000 );
    argv[ arg ] = responseTimeoutString;
    ++arg;

    /* Add maximum per hand timeout argument */
    argv[ arg ] = "--t_hand";
    ++arg;

    snprintf( handTimeoutString,
             sizeof( handTimeoutString ),
             "%d",
             (int)conf->handTimeoutSecs * 1000 );
    argv[ arg ] = handTimeoutString;
    ++arg;

    /* Add average per hand time argument */
    argv[ arg ] = "--t_per_hand";
    ++arg;

    snprintf( avgHandTimeString,
             sizeof( avgHandTimeString ),
             "%d",
             (int)conf->avgHandTimeSecs * 1000 );
    argv[ arg ] = avgHandTimeString;
    ++arg;


    argv[ arg ] = "-q";
    ++arg;

    /* Restore the appending behaviour so multiple matches get appended into
     * the same log file */
    argv[ arg ] = "-a";
    ++arg;

    argv[ arg ] = NULL;

    execv( BM_DEALER, argv );

    fprintf( stderr, "BM_ERROR: could not start dealer\n" );
    exit( EXIT_FAILURE );
  }

  /* parent has to talk to child to get ports */
  ssize_t r;
  int pos, t;
  fd_set readfds;
  struct timeval timeout;
  char portString[ READBUF_LEN ];

  close( stdoutPipe[ 1 ] );
  timeout.tv_sec = BM_DEALER_WAIT_SECS;
  timeout.tv_usec = 0;
  FD_ZERO( &readfds );
  FD_SET( stdoutPipe[ 0 ], &readfds );
  if( select( stdoutPipe[ 0 ] + 1, &readfds, NULL, NULL, &timeout ) < 1 ) {

    fprintf( stderr,
	     "BM_ERROR: timed out waiting for port string from dealer\n" );
    exit( EXIT_FAILURE );
  }
  r = read( stdoutPipe[ 0 ], portString, READBUF_LEN );
  if( r <= 0 || portString[ r - 1 ] != '\n' ) {

    fprintf( stderr, "BM_ERROR: could not read port string from dealer\n" );
    exit( EXIT_FAILURE );
  }
  portString[ r ] = 0;

  /* parse the port string */
  pos = 0;
  for( p = 0; p < match->gameConf->game->numPlayers; ++p ) {

    if( sscanf( &portString[ pos ],
		" %"SCNu16"%n",
		&job->ports[ p ],
		&t ) < 1 ) {

      fprintf( stderr,
	       "BM_ERROR: could not get port for player %d from dealer\n",
	       p + 1 );
      exit( EXIT_FAILURE );
    }
    pos += t;
  }
}

pid_t startBot( const ServerState *serv,
		const BotSpec *bot,
		const uint16_t port,
		const int botPosition )
{
  pid_t pid;

  pid = fork();
  if( pid < 0 ) {

    fprintf( stderr, "BM_ERROR: fork() failed\n" );
    exit( EXIT_FAILURE );
  }
  if( !pid ) {
    /* child runs the bot command */
    char portString[ 8 ];
    char posString[ 16 ];

    snprintf( portString, sizeof( portString ), "%"PRIu16, port );
    snprintf( posString, sizeof( posString ), "%d", botPosition );

    /* throw away bot output */
    dup2( serv->devnullfd, 1 );
    dup2( serv->devnullfd, 2 );

    execl( bot->command,
	   bot->command,
	   serv->hostname,
	   portString,
	   posString,
	   NULL );

    fprintf( stderr, "BM_ERROR: could not start bot %s\n", bot->command );
    exit( EXIT_FAILURE );
  }

  return pid;
}

int sendStartMessage( const ServerState *serv,
		      const MatchJob *job,
		      const Connection *conn,
		      const uint16_t port )
{
  int len;
  char msg[ strlen( serv->hostname ) + 12 + READBUF_LEN ];

  len = snprintf( msg, sizeof( msg ), "# RUNNING %s\n", job->tag );
  assert( len > 0 );
  if( write( conn->connBuf->fd, msg, len ) < len ) {

    fprintf( stderr, "BM_ERROR: short write to connection\n" );
    return -1;
  }

  len = snprintf( msg,
                  sizeof( msg ),
                  "RUN %s %"PRIu16"\n",
                  serv->hostname, port );
  assert( len > 0 );
  if( write( conn->connBuf->fd, msg, len ) < len ) {

    fprintf( stderr, "BM_ERROR: short write to connection\n" );
    return -1;
  }

  return 0;
}

MatchJob runMatchJob( const Config *conf,
		      const ServerState *serv,
		      LLPoolEntry *matchEntry,
		      const uint32_t rngSeed )
{
  int p, botPosition;
  MatchJob job;
  Match *match = (Match *)LLPoolGetItem( matchEntry );
  char tag[ READBUF_LEN ];

  job.matchEntry = matchEntry;

  /* make the tag from the match tag */
  snprintf( tag, sizeof( tag ), "%s.%s", match->user->name, match->tag );
  job.tag = strdup( tag );

  /* initialise all PIDs to 0 */
  job.dealerPID = 0;
  for( p = 0; p < match->gameConf->game->numPlayers; ++p ) {

    job.botPID[ p ] = 0;
  }

  /* start the dealer */
  startDealer( conf, match, &job, rngSeed );

  /* deal with all the players */
  botPosition = 0;
  for( p = 0; p < match->gameConf->game->numPlayers; ++p ) {

    if( match->players[ p ].isNetworkPlayer ) {
      /* send message with port to network player to start up */
      Connection *conn = (Connection*)LLPoolGetItem( match->players[ p ].entry );

      if( sendStartMessage( serv, &job, conn, job.ports[ p ] ) < 0 ) {
	/* abort the job... */

	fprintf( stderr, "BM_ERROR: aborting job\n" );

	kill( job.dealerPID, SIGTERM );
	while( p > 0 ) {
	  --p;

	  if( job.botPID[ p ] ) {

	    kill( job.botPID[ p ], SIGTERM );
	  }
	}

	return job;
      }
    } else {
      /* start up bot */

      job.botPID[ p ]
	= startBot( serv,
		    (BotSpec *)LLPoolGetItem( match->players[ p ].entry ),
		    job.ports[ p ],
		    botPosition );
      ++botPosition;
    }
  }

  return job;
}

int startMatchJob( const Config *conf, ServerState *serv )
{
  int running;
  LLPoolEntry *cur, *next, *best;
  Match *curMatch, *bestMatch;
  MatchJob job;

  /* automatically done adding things if we've got no more matches */
  if( serv->matches->numEntries == 0 ) {

    return 0;
  }

  /* how many bots are currently running */
  running = 0;
  for( cur = LLPoolFirstEntry( serv->jobs );
       cur != NULL; cur = LLPoolNextEntry( cur ) ) {
    MatchJob *job = (MatchJob *)LLPoolGetItem( cur );

    running += botsInMatch( (Match*)LLPoolGetItem( job->matchEntry ) );
  }

  /* pick the best match to start */
  best = 0;
  bestMatch = 0;
  for( cur = LLPoolFirstEntry( serv->matches ); cur != NULL; cur = next ) {
    next = LLPoolNextEntry( cur );
    curMatch = (Match *)LLPoolGetItem( cur );

    if( curMatch->isRunning ) {

      continue;
    }

    if( curMatch->numRuns <= 0 ) {
      /* match is finished - clean it up */

      free( curMatch->tag );
      LLPoolRemoveEntry( serv->matches, cur );
      continue;
    }

    if( curMatch->gameConf->maxRunningJobs
	&& curMatch->gameConf->curRunningJobs
	>= curMatch->gameConf->maxRunningJobs ) {
      /* cur refers to a match in a game which is currently too busy */

      continue;
    }

    if( best == 0
	|| timeIsEarlier( &curMatch->user->waitStart,
			  &bestMatch->user->waitStart )
	|| ( !timeIsEarlier( &bestMatch->user->waitStart,
			     &curMatch->user->waitStart )
	     && timeIsEarlier( &curMatch->queueTime,
			       &bestMatch->queueTime ) ) ) {

      best = cur;
      bestMatch = curMatch;
    }
  }

  /* return failure if we couldn't find a runnable job */
  if( best == NULL ) {

    return 0;
  }

  /* check if we have the space to run the bots */
  if( conf->maxRunningBots
      && botsInMatch( bestMatch ) + running > conf->maxRunningBots ) {

    return 0;
  }

  /* create the job */
  job = runMatchJob( conf,
		     serv,
		     best,
		     bestMatch->useRngForSeed
		     ? genrand_int32( &bestMatch->rng )
		     : bestMatch->rngSeed );
  assert( job.dealerPID );
  LLPoolAddItem( serv->jobs, &job );

  /* update status about running jobs */
  ++( bestMatch->gameConf->curRunningJobs );
  bestMatch->isRunning = 1;

  /* update the user */
  gettimeofday( &bestMatch->user->waitStart, NULL );

  /* update the match */
  --bestMatch->numRuns;
  gettimeofday( &bestMatch->queueTime, NULL );

  return 1;
}

void initServerState( const Config *conf, ServerState *serv )
{
  struct addrinfo hints, *info;
  uint16_t port;
  int hnm, r;
  char *hn;
  char ipstr[ INET6_ADDRSTRLEN ];

  serv->conns = newLLPool( sizeof( Connection ) );
  serv->matches = newLLPool( sizeof( Match ) );
  serv->jobs = newLLPool( sizeof( MatchJob ) );

  /* create the socket clients will connect to */
  port = conf->port;
  serv->listenSocket = getListenSocket( &port );
  if( serv->listenSocket < 0 ) {

    fprintf( stderr, "BM_ERROR: could not open socket for listening\n" );
    exit( EXIT_FAILURE );
  }
  printf( "starting server on port %"PRIu16"\n", conf->port );

  init_genrand( &serv->rng, time( NULL ) );

  hnm = sysconf( _SC_HOST_NAME_MAX );
  hn = (char*)malloc( hnm );
  assert( hn != 0 );
  if( gethostname( hn, hnm + 1 ) < 0 ) {

    fprintf( stderr, "BM_ERROR: could not get hostname\n" );
    exit( EXIT_FAILURE );
  }

  memset( &hints, 0, sizeof( hints ) );
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  if( ( r = getaddrinfo( hn, NULL, &hints, &info ) ) != 0 ) {

    fprintf( stderr,
	     "BM_ERROR: could not get address info for host %s\n",
	     hn );
    exit( 1 );
  }
  free( hn );

  /* Get an address for the server */
  void *addr;

  /* get the pointer to the address itself,
   * different fields in IPv4 and IPv6: */
  if ( info->ai_family == AF_INET ) {
    /* IPv4 */
    struct sockaddr_in *ipv4 = ( struct sockaddr_in * ) info->ai_addr;
    addr = &( ipv4->sin_addr );
  } else {
    /* IPv6 */
    struct sockaddr_in6 *ipv6 = ( struct sockaddr_in6 * ) info->ai_addr;
    addr = &( ipv6->sin6_addr );
  }

  /* convert the IP to a string and store it:*/
  inet_ntop( info->ai_family, addr, ipstr, sizeof( ipstr ) );
  serv->hostname = strdup( ipstr );

  freeaddrinfo( info ); /* free the linked list */

  serv->devnullfd = open( "/dev/null", O_WRONLY );
  if( serv->devnullfd < 0 ) {

    fprintf( stderr, "BM_ERROR: could not open /dev/null\n" );
    exit( EXIT_FAILURE );
  }
}

int checkIfJobFinished( MatchJob *job )
{
  int status, r, p, allDone;
  Match *match = (Match *)LLPoolGetItem( job->matchEntry );

  allDone = 1;

  if( job->dealerPID ) {

    r = waitpid( job->dealerPID, &status, WNOHANG );
    if( r < 0 ) {

      fprintf( stderr, "BM_ERROR: could not wait on child\n" );
      exit( EXIT_FAILURE );
    }
    if( r == job->dealerPID ) {

      job->dealerPID = 0;
    } else {

      allDone = 0;
    }
  }

  for( p = 0; p < match->gameConf->game->numPlayers; ++p ) {
    if( job->botPID[ p ] == 0 ) {
      continue;
    }

    r = waitpid( job->botPID[ p ], &status, WNOHANG );
    if( r < 0 ) {

      fprintf( stderr, "BM_ERROR: could not wait on child\n" );
      exit( EXIT_FAILURE );
    }
    if( r == job->botPID[ p ] ) {

      job->botPID[ p ] = 0;
    } else {

      allDone = 0;
    }
  }

  return allDone;
}

void finishedJob( ServerState *serv, LLPoolEntry *jobEntry )
{
  MatchJob *job = (MatchJob *)LLPoolGetItem( jobEntry );
  Match *match = (Match *)LLPoolGetItem( job->matchEntry );

  free( job->tag );
  --( match->gameConf->curRunningJobs );
  match->isRunning = 0;
  LLPoolRemoveEntry( serv->jobs, jobEntry );
}

int main( int argc, char **argv )
{
  Config conf;
  ServerState serv;
  int maxfd;
  fd_set readfds;
  LLPoolEntry *cur, *next;
  struct timeval tv;

  if( argc < 2 ) {

    printUsage( stderr );
    exit( EXIT_FAILURE );
  }

  /* Ignore SIGPIPE.  It seems that SIGPIPE can be raised when the underlying
   * IO fails with a SIGPIPE.  Unfortunately this causes the entire benchmark
   * server to crash and jobs are lost.  Ignore the signal to avoid death */
  /* ???: May also need to catch SIGCHLD */
  signal( SIGPIPE, SIG_IGN );

  /* use the config file */
  setDefaults( &conf );
  readConfig( argv[ 1 ], &conf );

  /* initialise server state */
  initServerState( &conf, &serv );

  /* main I/O loop */
  while( 1 ) {

    /* clean up any finished jobs */
    for( cur = LLPoolFirstEntry( serv.jobs ); cur != NULL; cur = next ) {
      next = LLPoolNextEntry( cur );
      MatchJob *job = (MatchJob *)LLPoolGetItem( cur );

      if( checkIfJobFinished( job ) ) {

	finishedJob( &serv, cur );
      }
    }

    /* clean up any closed connections */
    for( cur = LLPoolFirstEntry( serv.conns ); cur != NULL; cur = next ) {
      next = LLPoolNextEntry( cur );

      if( ( (Connection *)LLPoolGetItem( cur ) )->status == STATUS_CLOSED ) {

	LLPoolRemoveEntry( serv.conns, cur );
      }
    }

    /* start jobs, up to the maximum */
    while( startMatchJob( &conf, &serv ) );

    /* wait for input */
    FD_ZERO( &readfds );
    FD_SET( serv.listenSocket, &readfds );
    maxfd = serv.listenSocket;
    tv.tv_sec = BM_MAX_IOWAIT_SECS;
    tv.tv_usec = 0;
    for( cur = LLPoolFirstEntry( serv.conns );
	 cur != NULL; cur = LLPoolNextEntry( cur ) ) {
      Connection *conn = (Connection *)LLPoolGetItem( cur );

      FD_SET( conn->connBuf->fd, &readfds );
      if( conn->connBuf->fd > maxfd ) {

	maxfd = conn->connBuf->fd;
      }
    }
    if( select( maxfd + 1, &readfds, NULL, NULL, &tv ) < 0 ) {

      fprintf( stderr, "BM_ERROR: select failed\n" );
      exit( -1 );
    }

    /* process anything that's happened */
    if( FD_ISSET( serv.listenSocket, &readfds ) ) {

      handleListenSocket( &conf, &serv );
    }
    for( cur = LLPoolFirstEntry( serv.conns );
	 cur != NULL; cur = LLPoolNextEntry( cur ) ) {
      Connection *conn = (Connection *)LLPoolGetItem( cur );

      if( FD_ISSET( conn->connBuf->fd, &readfds ) ) {

	handleConnection( &conf, &serv, cur );
      }
    }
  }

  close( serv.listenSocket );
  return EXIT_SUCCESS;
}