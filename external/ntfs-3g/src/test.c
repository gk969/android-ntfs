#include <stdio.h>
#include <string.h>

//#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

//#define O_RDWR		00000000
//#define  __u64  unsigned long long 


#define  BUFFERSIZE  16


/*****************************************
*
*	   /bin/test   /media/usba1/
*
*****************************************/

int main(int argc,char* argv[])
{
	char *filestr = argv[1];
	int fd ;
	__s64 start= 0;
	__s32 total=0;
	__u32 start_h= 0;//, total_h=0;
	__u32 start_l= 0;//, total_l=0;
	__u8  buffer[BUFFERSIZE];
	
	if(argc < 4)
	{
		printf("< 4 args\n");   	
		printf("format:filestr start_h(0x):start_l size(0x)\n");   	
		return -1;
	}

	fd =open(filestr,O_RDWR|O_LARGEFILE,0);
	if(fd < 0)
	{
		printf("open %s fail\n",filestr);   	
		return -1;
	}

	sscanf(argv[2],"0x%x:%x",&start_h,&start_l);
	sscanf(argv[3],"0x%x",&total);

	start=((__s64)start_h<<32)+start_l;
	if(start < 0 || total< 0 )
	{
		printf("start or total <0 \n");   	
		return -1;
	}

	if(start!=lseek64(fd,start,SEEK_SET))
	{
		printf("lseek64 fail \n");   	
		return -1;
	}

	printf("read start from 0x%x:%x\n",start_h ,start_l );   	
	printf("total= 0x%x bytes\n",total );   	

	for ( ;total>0 ; total-=BUFFERSIZE)
	{
		int i;
		memset(buffer,0,BUFFERSIZE);	
		if(read(fd,buffer,BUFFERSIZE)<0)
		{
			printf("read at 0x%x%x fail\n",	(__u32)(start>>32),(__u32)(start&0xffffffff));   	
			printf("errno=%d\n",errno);   
			break;
		}
		
		for(i=0;i<BUFFERSIZE;i++)
		{
			printf("%02x ",buffer[i]);
		}
		printf("\n");

////////////////add  you code here////////////////////////////
			
	}	
	close(fd);
	return 0;		
}

