#import <Foundation/Foundation.h>
#include <arpa/inet.h>
#include <libproc.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static void check(BOOL condition, NSString *message) { if (!condition) @throw [NSException exceptionWithName:@"Check" reason:message userInfo:nil]; }
static NSData *json(id value) { return [NSJSONSerialization dataWithJSONObject:value options:0 error:nil]; }
static id parse(NSData *data) { return [NSJSONSerialization JSONObjectWithData:data options:0 error:nil]; }
static NSTask *task(NSArray *args, NSPipe **input, NSPipe **output) {
    NSTask *p=[NSTask new];p.executableURL=[NSURL fileURLWithPath:@"native/build/gtts-native" isDirectory:NO];p.arguments=args;
    NSPipe *in=[NSPipe pipe],*out=[NSPipe pipe];p.standardInput=in;p.standardOutput=out;
    [[NSFileManager defaultManager] createFileAtPath:@"native/tmp/check-engine.log" contents:nil attributes:nil];
    p.standardError=[NSFileHandle fileHandleForWritingAtPath:@"native/tmp/check-engine.log"];
    NSError *error=nil;check([p launchAndReturnError:&error],error.localizedDescription ?: @"Could not launch program");
    *input=in;*output=out;return p;
}
static NSData *readN(NSFileHandle *h,NSUInteger n) {
    NSMutableData *result=[NSMutableData data];while(result.length<n) {NSData *part=[h readDataOfLength:n-result.length];
        check(part.length>0,@"Unexpected end of worker output");[result appendData:part];} return result;
}
static NSDictionary *readLine(NSFileHandle *h) {
    NSMutableData *result=[NSMutableData data];for(;;){NSData *ch=readN(h,1);if(*(const char *)ch.bytes=='\n')break;[result appendData:ch];
        check(result.length<65536,@"Worker header is too long");}
    id value=parse(result);check([value isKindOfClass:[NSDictionary class]],@"Invalid worker JSON");return value;
}
static void sendLine(NSFileHandle *h,id value) {NSMutableData *data=[json(value) mutableCopy];[data appendBytes:"\n" length:1];[h writeData:data];}
static BOOL validWAV(NSString *path) {
    NSData *data=[NSData dataWithContentsOfFile:path];return data.length>100 && !memcmp(data.bytes,"RIFF",4) && !memcmp((const char *)data.bytes+8,"WAVE",4);
}
static int connectServer(int port) {
    int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0)return -1;
    struct timeval timeout={.tv_sec=3};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);
    struct sockaddr_in addr={.sin_len=sizeof addr,.sin_family=AF_INET,.sin_port=htons((uint16_t)port),.sin_addr={.s_addr=htonl(INADDR_LOOPBACK)}};
    if(connect(fd,(struct sockaddr *)&addr,sizeof addr)){close(fd);return -1;}return fd;
}
static NSData *request(int port,NSString *method,NSString *path,NSData *body,int *status) {
    int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0)return nil;
    struct timeval timeout={.tv_sec=30};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);
    struct sockaddr_in addr={.sin_len=sizeof addr,.sin_family=AF_INET,.sin_port=htons((uint16_t)port),.sin_addr={.s_addr=htonl(INADDR_LOOPBACK)}};
    if(connect(fd,(struct sockaddr *)&addr,sizeof addr)){close(fd);return nil;}
    NSString *header=[NSString stringWithFormat:@"%@ %@ HTTP/1.1\r\nHost: localhost\r\nContent-Length: %lu\r\nConnection: close\r\n\r\n",method,path,(unsigned long)body.length];
    NSMutableData *message=[[header dataUsingEncoding:NSUTF8StringEncoding] mutableCopy];if(body)[message appendData:body];
    const char *p=message.bytes;size_t left=message.length;while(left){ssize_t n=send(fd,p,left,0);if(n<=0){close(fd);return nil;}p+=n;left-=(size_t)n;}
    NSMutableData *reply=[NSMutableData data];uint8_t buffer[16384];ssize_t n;
    while((n=recv(fd,buffer,sizeof buffer,0))>0)[reply appendBytes:buffer length:(NSUInteger)n];close(fd);if(n<0 && !reply.length)return nil;
    NSData *separator=[@"\r\n\r\n" dataUsingEncoding:NSUTF8StringEncoding];NSRange mark=[reply rangeOfData:separator options:0 range:NSMakeRange(0,reply.length)];
    if(mark.location==NSNotFound)return nil;NSString *headers=[[NSString alloc] initWithData:[reply subdataWithRange:NSMakeRange(0,mark.location)] encoding:NSUTF8StringEncoding];
    if(sscanf(headers.UTF8String,"HTTP/%*s %d",status)!=1)return nil;
    NSUInteger start=mark.location+mark.length;unsigned long long expected=ULLONG_MAX;
    for(NSString *line in [headers componentsSeparatedByString:@"\r\n"])
        if([[line lowercaseString] hasPrefix:@"content-length:"])expected=[[[line substringFromIndex:15] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]] longLongValue];
    if(expected!=reply.length-start)return nil;return [reply subdataWithRange:NSMakeRange(start,reply.length-start)];
}
int main(void) {
    signal(SIGPIPE,SIG_IGN);
    @autoreleasepool {
        @try {
            NSString *home=getenv("HOME")?[NSString stringWithUTF8String:getenv("HOME")]:NSHomeDirectory();
            NSString *data=getenv("GTTS_DATA")?[NSString stringWithUTF8String:getenv("GTTS_DATA")]:[home stringByAppendingPathComponent:@"Library/Application Support/google-tts-local"];
            if(![data hasPrefix:@"/"])data=[[[NSFileManager defaultManager] currentDirectoryPath] stringByAppendingPathComponent:data];
            data=[data stringByStandardizingPath];setenv("GTTS_DATA",data.fileSystemRepresentation,1);
            NSString *lib=[data stringByAppendingPathComponent:@"libchrometts.so"],*pack=[data stringByAppendingPathComponent:@"voices/en-us-x-multi.zvoice"];
            check([[NSFileManager defaultManager] isReadableFileAtPath:lib] && [[NSFileManager defaultManager] isReadableFileAtPath:pack],@"Run gtts fetch --lang en-us before the integration check");
            NSArray *base=@[@"--lib",lib,@"--pack",pack];
            NSPipe *in,*out;NSTask *worker=task([@[@"worker"] arrayByAddingObjectsFromArray:base],&in,&out);
            NSFileHandle *writer=in.fileHandleForWriting,*reader=out.fileHandleForReading;
            NSDictionary *hello=readLine(reader);check([hello[@"speakers"][@"sfg"] isEqual:@"female"],@"Missing expected speaker");
            NSArray *speakerOrder=hello[@"speaker_order"];check([speakerOrder isKindOfClass:[NSArray class]] && speakerOrder.count==[hello[@"speakers"] count],@"Missing speaker order");
            for(int i=0;i<8;++i){sendLine(writer,@{@"text":@"Hello there.",@"speaker":@"sfg"});NSUInteger total=0;NSDictionary *h;
                do {h=readLine(reader);NSUInteger bytes=[h[@"bytes"] unsignedIntegerValue];check(!h[@"error"] && bytes%4==0,@"Invalid worker audio");
                    readN(reader,bytes);total+=bytes;} while([h[@"more"] boolValue]);
                check(total>1000,@"Invalid worker audio");}
            sendLine(writer,@{@"text":@"Hello",@"speaker":@"not-a-speaker"});check(readLine(reader)[@"error"]!=nil,@"Missing worker error");
            sendLine(writer,@{@"text":@"Again.",@"speaker":@"sfg"});NSUInteger total=0;NSDictionary *h;
            do {h=readLine(reader);NSUInteger bytes=[h[@"bytes"] unsignedIntegerValue];readN(reader,bytes);total+=bytes;} while([h[@"more"] boolValue]);
            check(total>1000,@"Worker did not recover");
            [writer closeFile];[worker waitUntilExit];check(worker.terminationStatus==0,@"Worker failed to stop");

            NSMutableArray *batchArgs=[NSMutableArray arrayWithObject:@"batch"];[batchArgs addObjectsFromArray:base];
            unlink("native/tmp/check-one.wav");unlink("native/tmp/check-two.wav");mode_t previous=umask(0022);
            NSTask *batch=task(batchArgs,&in,&out);umask(previous);writer=in.fileHandleForWriting;reader=out.fileHandleForReading;
            sendLine(writer,@{@"text":@"First sentence.",@"out":@"native/tmp/check-one.wav"});
            sendLine(writer,@{@"text":@"Second sentence.",@"out":@"native/tmp/check-two.wav",@"rate":@2});[writer closeFile];
            NSDictionary *first=readLine(reader),*second=readLine(reader);[batch waitUntilExit];
            check(batch.terminationStatus==0 && [first[@"seconds"] doubleValue]>0 && [second[@"seconds"] doubleValue]>0 &&
                  validWAV(@"native/tmp/check-one.wav") && validWAV(@"native/tmp/check-two.wav"),@"Batch output failed");
            struct stat wavStat;check(!stat("native/tmp/check-one.wav",&wavStat) && (wavStat.st_mode&0777)==0644,@"WAV creation mode did not honor umask");
            NSTask *invalid=task(batchArgs,&in,&out);uint8_t invalidBytes[]={0xff,'\n'};
            [in.fileHandleForWriting writeData:[NSData dataWithBytes:invalidBytes length:sizeof invalidBytes]];[in.fileHandleForWriting closeFile];
            [out.fileHandleForReading readDataToEndOfFile];[invalid waitUntilExit];check(invalid.terminationStatus!=0,@"Invalid UTF-8 batch input was ignored");

            NSString *jpPack=[data stringByAppendingPathComponent:@"voices/ja-jp-x-multi.zvoice"];
            if([[NSFileManager defaultManager] fileExistsAtPath:jpPack]) {
                NSTask *jp=task(@[@"speak",@"--lib",lib,@"--pack",jpPack,
                                  @"--speaker",@"htm",@"--text",@"こんにちは、お元気ですか。",@"--out",@"native/tmp/check-japanese.wav"],&in,&out);
                [in.fileHandleForWriting closeFile];[out.fileHandleForReading readDataToEndOfFile];[jp waitUntilExit];
                check(jp.terminationStatus==0 && validWAV(@"native/tmp/check-japanese.wav"),@"Japanese synthesis failed");
            }

            int port=16000+(getpid()%20000);NSMutableArray *serverArgs=[NSMutableArray arrayWithArray:@[@"serve",@"--port",[NSString stringWithFormat:@"%d",port],@"--speaker",@"not-a-speaker"]];[serverArgs addObjectsFromArray:base];
            NSTask *server=task(serverArgs,&in,&out);
            @try {
                int status=0;NSData *health=nil;for(int i=0;i<200 && !health;++i){health=request(port,@"GET",@"/health",nil,&status);if(!health)usleep(50000);}
                check(status==200 && [health isEqual:[@"{\"ok\": true}" dataUsingEncoding:NSUTF8StringEncoding]],@"Server did not become healthy");
                NSData *voices=request(port,@"GET",@"/voices",nil,&status);NSArray *catalog=parse(voices);
                check(status==200 && catalog.count==speakerOrder.count && [catalog[0][@"name"] isEqual:speakerOrder[0]],@"Voice catalog order failed");
                NSData *bad=request(port,@"POST",@"/speak",json(@{@"rate":@0,@"text":@"Hello"}),&status);check(status==400 && parse(bad)[@"error"],@"HTTP validation failed");
                NSData *method=request(port,@"PUT",@"/health",nil,&status);check(method && status==501,@"Unsupported HTTP method did not return 501");
                NSData *nullSpeaker=request(port,@"POST",@"/speak",json(@{@"text":@"Hello.",@"speaker":[NSNull null],@"format":@"f32"}),&status);
                check(status==200 && nullSpeaker.length>1000,@"HTTP null speaker did not use the default");
                NSLock *lock=[NSLock new];__block int failures=0;
                dispatch_apply(4,dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^(size_t i){(void)i;@autoreleasepool{
                    int code=0;NSData *audio=request(port,@"POST",@"/speak",json(@{@"text":@"Hello there.",@"format":@"f32"}),&code);
                    if(code!=200 || audio.length<1000 || audio.length%4){[lock lock];++failures;[lock unlock];}
                }});
                check(!failures,@"Concurrent HTTP synthesis failed");
                NSMutableString *longText=[NSMutableString string];for(int i=0;i<51200;++i)[longText appendString:@"0123456789abcdef"];
                NSData *invalidJob=json(@{@"text":longText,@"speaker":@"missing"});
                struct proc_taskinfo beforeInfo={0},afterInfo={0};
                for(int i=0;i<68;++i){@autoreleasepool{NSData *response=request(port,@"POST",@"/speak",invalidJob,&status);check(status==400 && parse(response)[@"error"],@"Repeated invalid HTTP request failed");}
                    if(i==7)check(proc_pidinfo(server.processIdentifier,PROC_PIDTASKINFO,0,&beforeInfo,sizeof beforeInfo)==sizeof beforeInfo,@"Could not measure server memory");}
                check(proc_pidinfo(server.processIdentifier,PROC_PIDTASKINFO,0,&afterInfo,sizeof afterInfo)==sizeof afterInfo,@"Could not measure server memory");
                check(afterInfo.pti_resident_size<=beforeInfo.pti_resident_size+32*1024*1024,@"Server memory grew after repeated failing requests");
                int oversized=connectServer(port);check(oversized>=0,@"Oversized request connection failed");
                const char over[]="POST /speak HTTP/1.1\r\nContent-Length: 1048577\r\n\r\n";
                check(send(oversized,over,sizeof over-1,0)==sizeof over-1,@"Oversized request write failed");
                char response[256]={0};ssize_t n=recv(oversized,response,sizeof response-1,0);close(oversized);
                check(n>0 && strstr(response," 400 "),@"Oversized request was accepted");
                int slow[16];const char partial[]="GET /health HTTP/1.1\r\nX-Slow: ";
                for(int i=0;i<16;++i){slow[i]=connectServer(port);check(slow[i]>=0 && send(slow[i],partial,sizeof partial-1,0)==sizeof partial-1,@"Slow connection setup failed");}
                usleep(500000);NSData *busy=request(port,@"GET",@"/health",nil,&status);check(busy && status==503,@"HTTP concurrency limit failed");
                for(int i=0;i<11;++i){sleep(1);if(send(slow[0],"x",1,0)<0)break;}
                memset(response,0,sizeof response);n=recv(slow[0],response,sizeof response-1,0);
                for(int i=0;i<16;++i)close(slow[i]);
                check(n>0 && strstr(response," 400 "),@"HTTP request deadline did not stop a slow client");
            } @finally { [server terminate];[server waitUntilExit]; }
            printf("Worker reuse, batch validation, voice order, HTTP limits, and synthesis: passed.\n");return 0;
        } @catch(NSException *error) {fprintf(stderr,"Check failed: %s\n",error.reason.UTF8String);return 1;}
    }
}
