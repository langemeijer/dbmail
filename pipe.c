/* $Id$
 * Functions for reading the pipe from the MTA */


#include "config.h"
#include "pipe.h"

#define HEADER_BLOCK_SIZE 1024
#define QUERY_SIZE 255

void create_unique_id(char *target, unsigned long messageid)
{
  time_t now;
  time(&now);
  trace (TRACE_DEBUG,"create_unique_id(): createding id",target);
  snprintf (target,UID_SIZE,"%luA%lu",messageid,now);
  trace (TRACE_DEBUG,"create_unique_id(): created: %s",target);
}
	

char *read_header(unsigned long *blksize)
     /* returns <0 on failure */
{
  /* reads incoming pipe until header is found */
  /* we're going to check every DB_READ_BLOCK_SIZE if header is read in memory */

  char *header, *strblock;
  int usedmem=0; 
  int end_of_header=0;
	
  memtst ((strblock = (char *)malloc(READ_BLOCK_SIZE))==NULL);
  memtst ((header = (char *)malloc(HEADER_BLOCK_SIZE))==NULL);

  /* here we will start a loop to read in the message header */
  /* the header will be everything up until \n\n or an EOF of */
  /* in_stream (stdin) */
	
  trace (TRACE_INFO, "read_header(): readheader start\n");

  while ((end_of_header==0) && (!feof(stdin)))
    {
      /* fgets will read until \n occurs */
      strblock = fgets (strblock, READ_BLOCK_SIZE, stdin);
		if (strblock)
			usedmem += (strlen(strblock)+1);
	
      /* If this happends it's a very big header */	
      if (usedmem>HEADER_BLOCK_SIZE)
	memtst(((char *)realloc(header,usedmem))==NULL);
		
      /* now we concatenate all we have to the header */
		if (strblock)
			memtst((header=strcat(header,strblock))==NULL);

      /* check if the end of header has occured */
      if (strstr(header,"\n\n")!=NULL)
	{
	  /* we've found the end of the header */
	  trace (TRACE_DEBUG,"read_header(): end header found\n");
	  end_of_header=1;
	}
		
      /* reset strlen to 0 */
		if (strblock)
			strblock[0]='\0';
    }
	
  trace (TRACE_INFO, "read_header(): readheader done");
  trace (TRACE_DEBUG, "read_header(): found header [%s]",header);
  trace (TRACE_DEBUG, "read_header(): header size [%d]",strlen(header));	
  free(strblock);
	
  if (usedmem==0)
    {
      free(strblock);
      free(header);
      trace (TRACE_STOP, "read_header(): not a valid mailheader found\n");
      *blksize=0;
    }
  else
    *blksize=strlen(header);

  trace (TRACE_INFO, "read_header(): function successfull\n");
  return header;
}

int insert_messages(char *header, unsigned long headersize, struct list *users)
{
  /* 	this loop gets all the users from the list 
	and check if they're in the database */

  struct element *tmp;
  char *insertquery;
  char *updatequery;
  char *unique_id;
  char *strblock;
  char *domain, *ptr;
  char *tmpbuffer=NULL;
  size_t usedmem=0, totalmem=0;
  struct list userids;
  struct list messageids;
  struct list external_forwards;
  struct list bounces;
  unsigned long temp_message_record_id,userid;
  int i;
  FILE *instream = stdin;
  
  /* step 1.
     inserting first message
     first insert the header into the database
     the result is the first message block
     next create a message record
     update the first block with the messagerecord id number
     add the rest of the messages
     update the last and the total memory field*/
	
  /* creating a message record for the user */
  /* all email users to which this message is sent much receive this */

	
  memtst((insertquery = (char *)malloc(QUERY_SIZE))==NULL);
  memtst((updatequery = (char *)malloc(QUERY_SIZE))==NULL);
  memtst((unique_id = (char *)malloc(UID_SIZE))==NULL);

  /* initiating list with userid's */
  list_init(&userids);

  /* initiating list with messageid's */
  list_init(&messageids);
	
  /* initiating list with external forwards */
  list_init(&external_forwards);

  /* initiating list with bounces */
  list_init (&bounces);
	
  /* get the first target address */
  tmp=list_getstart(users);

  while (tmp!=NULL)
    {
      /* loops all mailusers and adds them to the list */
      /* db_check_user(): returns a list with character array's containing 
       * either userid's or forward addresses 
       */
      db_check_user((char *)tmp->data,&userids);
      trace (TRACE_DEBUG,"insert_messages(): user [%s] found total of [%d] aliases",(char *)tmp->data,
	     userids.total_nodes);
      
      if (userids.total_nodes==0) /* userid's found */
	{
	  /* I needed to change this because my girlfriend said so
	     and she was actually right. Domain forwards are last resorts
	     if a delivery cannot be found with an existing address then
	     and only then we need to check if there are domain delivery's */
			
	  trace (TRACE_INFO,"insert_messages(): no users found to deliver to. Checking for domain forwards");	
			
	  domain=strchr((char *)tmp->data,'@');

	  if (domain!=NULL)	/* this should always be the case! */
	    {
	      trace (TRACE_DEBUG,"insert_messages(): checking for domain aliases. Domain = [%s]",domain);
				/* checking for domain aliases */
	      db_check_user(domain,&userids);
	      trace (TRACE_DEBUG,"insert_messages(): domain [%s] found total of [%d] aliases",domain,
		     userids.total_nodes);
	    }
	}
    
      /* user does not exists in aliases tables
	 so bounce this message back with an error message */
      if (userids.total_nodes==0)
	{
	  /* still no effective deliveries found, create bouncelist */
	  list_nodeadd(&bounces, tmp->data, strlen(tmp->data)+1);
	}

      /* get the next taget in list */
      tmp=tmp->nextnode;
    }
		
  /* get first target uiserid */
  tmp=list_getstart(&userids);

  while (tmp!=NULL)
    {	
      /* traversing list with userids and creating a message for each userid */
		
      /* checking if tmp->data is numeric. If so, we should try to 
       * insert to that address in the database 
       * else we need to forward the message 
       * ---------------------------------------------------------
       * FIXME: The id needs to be checked!, it might be so that it is set in the 
       * virtual user table but that doesn't mean it's valid! */

      trace (TRACE_DEBUG,"insert_messages(): alias deliver_to is [%s]",
	     (char *)tmp->data);
		
      ptr=(char *)tmp->data;
      i = 0;
		
      while (isdigit(ptr[0]))
	{
	  i++;
	  ptr++;
	}
		
      if (i<strlen((char *)tmp->data))
	{
	  /* FIXME: it's probably a forward to another address
	   * we need to do an email address validity test if the first char !| */
	  trace (TRACE_DEBUG,"insert_messages(): no numeric value in deliver_to, calling external_forward");

			/* creating a list of external forward addresses */
	  list_nodeadd(&external_forwards,(char *)tmp->data,strlen(tmp->data)+1);
	}
      else
	{
	  /* make the id numeric */
	  userid=atol((char *)tmp->data);

	  /* create a message record */
	  temp_message_record_id=db_insert_message ((unsigned long *)&userid);

	  /* message id is an array of returned message id's
	   * all messageblks are inserted for each message id
	   * we could change this in the future for efficiency
	   * still we would need a way of checking which messageblks
	   * belong to which messages */
		
	  /* adding this messageid to the message id list */
	  list_nodeadd(&messageids,&temp_message_record_id,sizeof(temp_message_record_id));
		
	  /* adding the first header block per user */
	  db_insert_message_block (header,temp_message_record_id);
	}
      /* get next item */	
      tmp=tmp->nextnode;
    }

  trace(TRACE_MESSAGE,"insert_messages(): we need to deliver [%lu] messages to external addresses",
	list_totalnodes(&external_forwards));
	
  
  /* reading rest of the pipe and creating messageblocks 
   * we need to create a messageblk for each messageid */

  trace (TRACE_DEBUG,"insert_messages(): allocating [%d] bytes of memory for readblock",READ_BLOCK_SIZE);

  memtst ((strblock = (char *)malloc(READ_BLOCK_SIZE))==NULL);
	
	/* first we need to check if we need to deliver into the database */
	if (list_totalnodes(&messageids)>0)
	{
		totalmem = 0; /* reset totalmem counter */
		/* we have local deliveries */ 
		while (!feof(instream))
		{
			trace (TRACE_DEBUG,"errorstatus : [%d]",ferror(instream));
			usedmem = fread (strblock, sizeof(char), READ_BLOCK_SIZE, instream);
		
			/* fread won't do this for us! */	
			if (strblock)
				strblock[usedmem]='\0';
			
			if (usedmem>0) /* usedmem is 0 with an EOF */
			{
				totalmem = totalmem + usedmem;
			
				tmp=list_getstart(&messageids);
				while (tmp!=NULL)
				{
					db_insert_message_block (strblock,*(unsigned long *)tmp->data);
					tmp=tmp->nextnode;
				}
				
				/* resetting strlen for strblock */
				strblock[0]='\0';
				usedmem = 0;
				
			}
			else 
				trace (TRACE_DEBUG, "insert_messages(): end of instream stream");
		
	
		}; 
		
      trace (TRACE_DEBUG,"insert_messages(): updating size fields");
	

		/* we need to update messagesize in all messages */
      tmp=list_getstart(&messageids);
      while (tmp!=NULL)
		{
			/* we need to create a unique id per message 
			* we're using the messageidnr for this, it's unique 
			* a special field is created in the database for other possible 
			* even more unique strings */
			create_unique_id(unique_id,*(unsigned long*)tmp->data); 
			db_update_message ((unsigned long*)tmp->data,unique_id,totalmem+headersize);
			trace (TRACE_MESSAGE,"insert_messages(): message id=%lu, size=%lu is inserted",
			*(unsigned long*)tmp->data, totalmem+headersize);
			temp_message_record_id=*(unsigned long*)tmp->data;
			tmp=tmp->nextnode;
		}
	}

  /* handle all bounced messages */
  if (list_totalnodes(&bounces)>0)
    {
      /* bouncing invalid messages */
      trace (TRACE_DEBUG,"insert_messages(): sending bounces");
      tmp=list_getstart(&bounces);
      while (tmp!=NULL)
	{	
	  bounce (header,(char *)tmp->data,BOUNCE_NO_SUCH_USER);
	  tmp=tmp->nextnode;	
	}
    }

  /* do we have forward addresses ? */
	if (list_totalnodes(&external_forwards)>0)
	{
		/* sending the message to forwards */
  
		trace (TRACE_DEBUG,"insert_messages(): delivering to external addresses");
  
		if (list_totalnodes(&messageids)==0)
		{
			/* deliver using stdin */
			pipe_forward (stdin, &external_forwards, header, 0);
		}
		else
		{
			/* deliver using database */
			tmp = list_getstart(&messageids);
			pipe_forward (stdin, &external_forwards, header, *((unsigned long *)tmp->data));
		}
	}
	
  trace (TRACE_DEBUG,"insert_messages(): Freeing memory blocks");
  /* memory cleanup */
  if (tmpbuffer!=NULL)
    {
      trace (TRACE_DEBUG,"insert_messages(): tmpbuffer freed");
      free(tmpbuffer);
    }
  trace (TRACE_DEBUG,"insert_messages(): header freed");
  free(header);
  trace (TRACE_DEBUG,"insert_messages(): uniqueid freed");
  free(unique_id);
  trace (TRACE_DEBUG,"insert_messages(): strblock freed");
  free (strblock);
  trace (TRACE_DEBUG,"insert_messages(): insertquery freed");
  free(insertquery);
  trace (TRACE_DEBUG,"insert_messages(): updatequery freed");
  free(updatequery);
  trace (TRACE_DEBUG,"insert_messages(): End of function");
  
  list_freelist(&bounces.start);
  list_freelist(&userids.start);
  list_freelist(&messageids.start);
  list_freelist(&external_forwards.start);
  
  return 0;
}
