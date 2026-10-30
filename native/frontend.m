#import <Foundation/Foundation.h>
#import <CommonCrypto/CommonDigest.h>
#import <curl/curl.h>
#include "third_party/zstd.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <mach-o/dyld.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
#ifndef GTTS_VERSION
#define GTTS_VERSION "0.4.0"
#endif

static NSString *const packageURL = @"https://storage.googleapis.com/chromeos-localmirror/distfiles/googletts-35.3.tar.xz";
static NSString *const packageHash = @"51785a17918e2c4d9aa2f4cc17ed8ee672b0127501a58b457fdd7876755d046a";
static NSString *engineExecutable;
static NSString *findEngine(void);
static NSArray<NSString *> *languages(void) { return @[@"en-us", @"es-es", @"fr-fr", @"hi-in", @"ja-jp", @"nl-nl"]; }
static void raiseError(NSString *message) { @throw [NSException exceptionWithName:@"GTTS" reason:message userInfo:nil]; }
static NSData *jsonData(id value) {
    NSError *error = nil; NSData *data = [NSJSONSerialization dataWithJSONObject:value options:0 error:&error];
    if (!data) raiseError(error.localizedDescription ?: @"Could not encode JSON"); return data;
}
static id parseJSON(NSData *data) {
    NSError *error = nil; id value = [NSJSONSerialization JSONObjectWithData:data options:NSJSONReadingFragmentsAllowed error:&error];
    if (!value) raiseError(error.localizedDescription ?: @"Invalid JSON"); return value;
}
static NSString *pathString(NSString *path) {
    if ([path isEqualToString:@"~"] || [path hasPrefix:@"~/"]) {
        NSString *home=getenv("HOME") ? [NSString stringWithUTF8String:getenv("HOME")] : NSHomeDirectory();
        path=[home stringByAppendingString:[path substringFromIndex:1]];
    }
    return [path stringByStandardizingPath];
}
static NSString *absolutePath(NSString *path) {
    if(![path isKindOfClass:[NSString class]] || !path.length)raiseError(@"Path cannot be empty");
    path=pathString(path);if(![path hasPrefix:@"/"])path=[[[NSFileManager defaultManager] currentDirectoryPath] stringByAppendingPathComponent:path];
    return [path stringByStandardizingPath];
}
static BOOL safeFileTarget(NSString *path) {
    struct stat st;if(!lstat(path.fileSystemRepresentation,&st))return !S_ISLNK(st.st_mode) && (!S_ISREG(st.st_mode) || st.st_nlink==1);
    return errno==ENOENT;
}
static void ensureDirectory(NSString *path) {
    NSError *error = nil;
    if (![[NSFileManager defaultManager] createDirectoryAtPath:path withIntermediateDirectories:YES attributes:nil error:&error])
        raiseError(error.localizedDescription ?: @"Cannot create data directory");
}
static void writeAllFD(int fd, const void *bytes, size_t length) {
    const uint8_t *p = bytes;
    while (length) { ssize_t n = write(fd, p, length); if (n < 0 && errno == EINTR) continue;
        if (n <= 0) raiseError([NSString stringWithFormat:@"Write failed: %s", strerror(errno)]); p += n; length -= (size_t)n; }
}
static void atomicWrite(NSString *path, NSData *data) {
    NSString *parent = [pathString(path) stringByDeletingLastPathComponent]; if (!parent.length) parent = @".";
    struct stat existing;BOOL hasExisting=!lstat(path.fileSystemRepresentation,&existing);
    if (!safeFileTarget(path) || ![[NSFileManager defaultManager] fileExistsAtPath:parent])
        raiseError(@"Output must have an existing parent directory and not be a symlink or hard link");
    char *temp=strdup([[parent stringByAppendingPathComponent:@".gtts-output-XXXXXX"] fileSystemRepresentation]);
    int fd = temp ? mkstemp(temp) : -1;
    if (fd < 0) {free(temp);raiseError(@"Cannot create temporary output");}
    @try {
        if(close(fd)){fd=-1;raiseError(@"Cannot prepare temporary output");}fd=-1;
        if(unlink(temp))raiseError(@"Cannot prepare temporary output");
        fd=open(temp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0666);
        if(fd<0 || (hasExisting && fchmod(fd,existing.st_mode & 07777)))raiseError(@"Cannot create output with the requested permissions");
        writeAllFD(fd, data.bytes, data.length); if (close(fd)) { fd = -1; raiseError(@"Cannot close output"); } fd = -1;
        if (!safeFileTarget(path) || rename(temp, path.fileSystemRepresentation)) raiseError([NSString stringWithFormat:@"Cannot write %@: %s", path, strerror(errno)]);
    } @finally { if (fd >= 0) close(fd); unlink(temp); free(temp); }
}
static void put16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void put32(uint8_t *p, uint32_t v) { put16(p,(uint16_t)v); put16(p+2,(uint16_t)(v>>16)); }
static NSData *wavData(NSData *audio) {
    size_t samples = audio.length/sizeof(float); if (samples > (UINT32_MAX-36)/2) raiseError(@"Audio is too long for WAV");
    NSMutableData *out = [NSMutableData dataWithLength:44+samples*2]; uint8_t *p = out.mutableBytes;
    memcpy(p,"RIFF",4); put32(p+4,(uint32_t)(36+samples*2)); memcpy(p+8,"WAVEfmt ",8); put32(p+16,16);
    put16(p+20,1); put16(p+22,1); put32(p+24,24000); put32(p+28,48000); put16(p+32,2); put16(p+34,16);
    memcpy(p+36,"data",4); put32(p+40,(uint32_t)(samples*2));
    const float *src = audio.bytes;
    for (size_t i=0; i<samples; ++i) { double n = (double)src[i]*32767.0;
        int value = !isfinite(n) ? 0 : n >= 32767 ? 32767 : n <= -32768 ? -32768 : (int)n; put16(p+44+i*2,(uint16_t)(int16_t)value); }
    return out;
}
static size_t curlWrite(char *ptr, size_t size, size_t count, void *opaque) { return fwrite(ptr, size, count, (FILE *)opaque) * size; }
static NSString *sha256File(NSString *path) {
    FILE *f = fopen(path.fileSystemRepresentation,"rb"); if (!f) raiseError(@"Cannot open downloaded package");
    CC_SHA256_CTX ctx; CC_SHA256_Init(&ctx); uint8_t buffer[65536], digest[CC_SHA256_DIGEST_LENGTH]; size_t n;
    while ((n=fread(buffer,1,sizeof buffer,f))) CC_SHA256_Update(&ctx, buffer, (CC_LONG)n);
    int err=ferror(f); fclose(f); if(err) raiseError(@"Cannot read downloaded package"); CC_SHA256_Final(digest,&ctx);
    NSMutableString *hex=[NSMutableString string]; for(size_t i=0;i<sizeof digest;++i) [hex appendFormat:@"%02x",digest[i]]; return hex;
}
static int runProcess(NSArray<NSString *> *args) {
    char **argv=calloc(args.count+1,sizeof *argv); if(!argv) raiseError(@"Out of memory");
    for(NSUInteger i=0;i<args.count;++i) argv[i]=(char *)[args[i] fileSystemRepresentation];
    pid_t pid; int result=posix_spawn(&pid,argv[0],NULL,NULL,argv,environ); free(argv);
    if(result) raiseError([NSString stringWithFormat:@"Cannot start process: %s",strerror(result)]);
    int status; while(waitpid(pid,&status,0)<0) if(errno!=EINTR) raiseError(@"Cannot wait for process");
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
static void download(NSString *url, NSString *destination, NSString *digest) {
    FILE *out=fopen(destination.fileSystemRepresentation,"wb");if(!out)raiseError(@"Cannot create download file");
    CURL *curl=curl_easy_init();if(!curl){fclose(out);raiseError(@"Cannot initialize curl");}
    curl_easy_setopt(curl,CURLOPT_URL,url.UTF8String);curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,1L);
    curl_easy_setopt(curl,CURLOPT_FAILONERROR,1L);curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,curlWrite);
    curl_easy_setopt(curl,CURLOPT_WRITEDATA,out);curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,30L);
    CURLcode rc=curl_easy_perform(curl);curl_easy_cleanup(curl);int closeError=fclose(out);
    if(rc!=CURLE_OK || closeError)raiseError([NSString stringWithFormat:@"Download failed: %s",curl_easy_strerror(rc)]);
    if(![[sha256File(destination) lowercaseString] isEqualToString:digest])raiseError([NSString stringWithFormat:@"SHA-256 mismatch for %@",url]);
}
static void extractMember(NSString *archive,NSString *member,NSString *output) {
    int fd=open(output.fileSystemRepresentation,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);
    if(fd<0)raiseError(@"Cannot create extraction file");
    posix_spawn_file_actions_t actions;posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions,fd,1);posix_spawn_file_actions_addinherit_np(&actions,2);
    posix_spawnattr_t attr;posix_spawnattr_init(&attr);posix_spawnattr_setflags(&attr,POSIX_SPAWN_CLOEXEC_DEFAULT);
    char *args[]={(char *)"/usr/bin/tar",(char *)"-xOf",(char *)archive.fileSystemRepresentation,(char *)member.fileSystemRepresentation,NULL};
    pid_t pid;int result=posix_spawn(&pid,args[0],&actions,&attr,args,environ);close(fd);
    posix_spawn_file_actions_destroy(&actions);posix_spawnattr_destroy(&attr);
    if(result)raiseError([NSString stringWithFormat:@"Cannot extract package: %s",strerror(result)]);
    int status;while(waitpid(pid,&status,0)<0)if(errno!=EINTR)raiseError(@"Cannot wait for package extraction");
    if(!WIFEXITED(status)||WEXITSTATUS(status))raiseError([NSString stringWithFormat:@"Package member missing: %@",member]);
}
static void decompressZstd(NSString *source,NSString *destination) {
    FILE *in=fopen(source.fileSystemRepresentation,"rb");
    int fd=open(destination.fileSystemRepresentation,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);
    FILE *out=fd>=0?fdopen(fd,"wb"):NULL;ZSTD_DStream *stream=ZSTD_createDStream();
    if(!in || !out || !stream) {if(in)fclose(in);if(out)fclose(out);else if(fd>=0)close(fd);if(stream)ZSTD_freeDStream(stream);raiseError(@"Cannot prepare runtime extraction");}
    @try {
        size_t remaining=ZSTD_initDStream(stream),total=0,n;uint8_t inputBytes[65536],outputBytes[65536];
        if(ZSTD_isError(remaining))raiseError(@"Cannot initialize runtime decompression");
        while((n=fread(inputBytes,1,sizeof inputBytes,in))>0) {
            ZSTD_inBuffer input={inputBytes,n,0};
            while(input.pos<input.size) {
                ZSTD_outBuffer output={outputBytes,sizeof outputBytes,0};
                size_t before=input.pos;
                remaining=ZSTD_decompressStream(stream,&output,&input);
                if(ZSTD_isError(remaining) || output.pos > 64*1024*1024-total || (input.pos==before && !output.pos))raiseError(@"Invalid or oversized runtime archive");
                if(fwrite(outputBytes,1,output.pos,out)!=output.pos)raiseError(@"Cannot write runtime archive");
                total+=output.pos;
            }
        }
        if(ferror(in)||remaining!=0||fflush(out))raiseError(@"Incomplete runtime archive");
    } @finally {fclose(in);fclose(out);ZSTD_freeDStream(stream);}
}
static void fetchRuntime(NSString *dataDir,NSString *stage) {
    // SHA-256 values are from Ubuntu's noble-updates arm64 package indices.
    NSArray<NSDictionary *> *packages=@[
      @{@"name":@"libc++.so.1",@"deb":@"pool/universe/l/llvm-toolchain-18/libc%2B%2B1-18_18.1.3-1ubuntu1_arm64.deb",
        @"sha":@"309c5d8419c67f1fac4e5b2e419afe792c8b37730acc9c20c83a07d0c32e3f92",@"libsha":@"b9f98132ddb21760eff7f31003bcce1622e790c6e7857cba13f4b0779835103f",
        @"member":@"./usr/lib/llvm-18/lib/libc++.so.1.0",@"copyright":@"./usr/share/doc/libc++1-18/copyright"},
      @{@"name":@"libc++abi.so.1",@"deb":@"pool/universe/l/llvm-toolchain-18/libc%2B%2Babi1-18_18.1.3-1ubuntu1_arm64.deb",
        @"sha":@"c4e83c341b1df1a70213b13c3a14c5047ab18c97e92eed63e5da7e2e2a9e3d2f",@"libsha":@"f1ffaa6c995179ea1521aa9a5ba62e25b2a4fcb60f43948eb9d8db36af613c2d",
        @"member":@"./usr/lib/llvm-18/lib/libc++abi.so.1.0",@"copyright":@"./usr/share/doc/libc++abi1-18/copyright"},
      @{@"name":@"libunwind.so.1",@"deb":@"pool/universe/l/llvm-toolchain-18/libunwind-18_18.1.3-1ubuntu1_arm64.deb",
        @"sha":@"be5eff7eaa52c8d88e2a779ff305700708bbaa0753a4290221e44e61342c3cac",@"libsha":@"9c64a3aba515c7a2e507cfa2714c6e3879dd153f9a90e2cb2f1062463cf3968f",
        @"member":@"./usr/lib/llvm-18/lib/libunwind.so.1.0",@"copyright":@"./usr/share/doc/libunwind-18/copyright"},
      @{@"name":@"libgcc_s.so.1",@"deb":@"pool/main/g/gcc-14/libgcc-s1_14.2.0-4ubuntu2~24.04.1_arm64.deb",
        @"sha":@"5b191f79ad985a9a653dbba45214619f729f9178a9bdf92f7c68db06c50d070b",@"libsha":@"f2d3ad2bf0b61f6bc944cc37d7b6ab7f88d2582b41ff989ca164803f56cc5f20",
        @"member":@"./usr/lib/aarch64-linux-gnu/libgcc_s.so.1"}
    ];
    NSString *runtime=[dataDir stringByAppendingPathComponent:@"runtime"];ensureDirectory(runtime);
    for(NSDictionary *package in packages) {
        NSString *dest=[runtime stringByAppendingPathComponent:package[@"name"]];
        NSString *existingNotice=[runtime stringByAppendingPathComponent:[package[@"name"] stringByAppendingString:@".copyright"]];
        if([[NSFileManager defaultManager] isReadableFileAtPath:dest] && [[sha256File(dest) lowercaseString] isEqualToString:package[@"libsha"]] &&
           (!package[@"copyright"] || [[NSFileManager defaultManager] isReadableFileAtPath:existingNotice]))continue;
        NSString *deb=[stage stringByAppendingPathComponent:@"runtime.deb"],*compressed=[stage stringByAppendingPathComponent:@"runtime.tar.zst"];
        NSString *archive=[stage stringByAppendingPathComponent:@"runtime.tar"];
        NSString *lib=[stage stringByAppendingPathComponent:@"runtime-library"];
        NSString *url=[@"https://ports.ubuntu.com/ubuntu-ports/" stringByAppendingString:package[@"deb"]];
        fprintf(stderr,"Downloading runtime %s\n",[package[@"name"] UTF8String]);download(url,deb,package[@"sha"]);
        extractMember(deb,@"data.tar.zst",compressed);decompressZstd(compressed,archive);extractMember(archive,package[@"member"],lib);
        if(![[sha256File(lib) lowercaseString] isEqualToString:package[@"libsha"]])raiseError(@"Runtime library SHA-256 mismatch");
        if(!safeFileTarget(dest)||rename(lib.fileSystemRepresentation,dest.fileSystemRepresentation))raiseError(@"Cannot install runtime library");
        if(package[@"copyright"]) {
            NSString *notice=[stage stringByAppendingPathComponent:@"runtime-copyright"];
            extractMember(archive,package[@"copyright"],notice);
            NSString *target=[runtime stringByAppendingPathComponent:[package[@"name"] stringByAppendingString:@".copyright"]];
            if(!safeFileTarget(target)||rename(notice.fileSystemRepresentation,target.fileSystemRepresentation))raiseError(@"Cannot install runtime license notice");
        }
    }
}
static void fetch(NSString *dataDir, NSArray<NSString *> *langs) {
    for(NSString *lang in langs) if(![languages() containsObject:lang]) raiseError([NSString stringWithFormat:@"Unsupported language: %@",lang]);
    ensureDirectory(dataDir);NSString *voices=[dataDir stringByAppendingPathComponent:@"voices"];ensureDirectory(voices);
    NSString *tmp=[dataDir stringByAppendingPathComponent:@"tmp"];ensureDirectory(tmp);
    char *stage=strdup([[tmp stringByAppendingPathComponent:@"fetch-XXXXXX"] fileSystemRepresentation]);
    if(!stage || !mkdtemp(stage)){free(stage);raiseError(@"Cannot create extraction directory");}
    @try {
        NSString *folder=[NSString stringWithUTF8String:stage],*archive=[folder stringByAppendingPathComponent:@"googletts.tar.xz"];
        fprintf(stderr,"Downloading %s\n",packageURL.UTF8String);download(packageURL,archive,packageHash);
        NSMutableArray *args=[NSMutableArray arrayWithArray:@[@"/usr/bin/tar",@"-xJf",archive,@"-C",folder,@"libchrometts_arm64.so"]];
        for(NSString *lang in langs)[args addObject:[lang stringByAppendingString:@"-x-multi.zvoice"]];
        if(runProcess(args))raiseError(@"Package extraction failed");
        NSMutableArray *names=[NSMutableArray arrayWithObject:@"libchrometts_arm64.so"];
        for(NSString *lang in langs)[names addObject:[lang stringByAppendingString:@"-x-multi.zvoice"]];
        fetchRuntime(dataDir,folder);
        for(NSString *name in names) {
            NSString *source=[folder stringByAppendingPathComponent:name];struct stat st;
            if(lstat(source.fileSystemRepresentation,&st)||!S_ISREG(st.st_mode)||st.st_nlink!=1)raiseError([NSString stringWithFormat:@"Invalid package member: %@",name]);
            NSString *dest=[name isEqualToString:@"libchrometts_arm64.so"]?[dataDir stringByAppendingPathComponent:@"libchrometts.so"]:[voices stringByAppendingPathComponent:name];
            if(!safeFileTarget(dest)||rename(source.fileSystemRepresentation,dest.fileSystemRepresentation))raiseError([NSString stringWithFormat:@"Cannot install %@",name]);
        }
        NSString *tools=[findEngine() stringByDeletingLastPathComponent];
        NSString *converter=[tools stringByAppendingPathComponent:@"gtts-macho-convert.py"];
        NSString *shim=[tools stringByAppendingPathComponent:@"libgtts-shim.dylib"];
        NSString *converted=[folder stringByAppendingPathComponent:@"macho-new"];
        NSString *destination=[dataDir stringByAppendingPathComponent:@"macho"];
        NSString *backup=[folder stringByAppendingPathComponent:@"macho-old"];
        if(![[NSFileManager defaultManager] isReadableFileAtPath:converter] ||
           ![[NSFileManager defaultManager] isReadableFileAtPath:shim])raiseError(@"Mach-O converter is missing; reinstall gtts.");
        setenv("PYTHONDONTWRITEBYTECODE","1",1);
        fprintf(stderr,"Converting downloaded engine and runtimes to Mach-O dylibs\n");
        if(runProcess(@[@"/usr/bin/python3",converter,@"--data",dataDir,@"--output",converted,@"--shim",shim]))
            raiseError(@"Mach-O conversion failed; the in-process and VM fallbacks remain available.");
        struct stat old; BOOL hadOld=!lstat(destination.fileSystemRepresentation,&old);
        if(hadOld && (!S_ISDIR(old.st_mode) || rename(destination.fileSystemRepresentation,backup.fileSystemRepresentation)))
            raiseError(@"Cannot replace the previous Mach-O conversion");
        if(rename(converted.fileSystemRepresentation,destination.fileSystemRepresentation)) {
            if(hadOld)rename(backup.fileSystemRepresentation,destination.fileSystemRepresentation);
            raiseError(@"Cannot install the Mach-O conversion");
        }
        fprintf(stderr,"Ready: %s/libchrometts.so and %s voices\n",dataDir.UTF8String,[[langs componentsJoinedByString:@", "] UTF8String]);
    } @finally {[[NSFileManager defaultManager] removeItemAtPath:[NSString stringWithUTF8String:stage] error:nil];free(stage);}
}

static void requireRuntime(NSString *data) {
    for(NSString *name in @[@"libc++.so.1",@"libc++abi.so.1",@"libgcc_s.so.1",@"libunwind.so.1"]) {
        NSString *path=[[data stringByAppendingPathComponent:@"runtime"] stringByAppendingPathComponent:name];
        if(![[NSFileManager defaultManager] isReadableFileAtPath:path])raiseError([NSString stringWithFormat:@"Runtime library %@ is missing. Run gtts fetch.",path]);
    }
}

static double monotonicTime(void) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}
static double workerDeadline(void) {
    const char *setting=getenv("GTTS_WORKER_TIMEOUT_MS");char *end=NULL;
    long ms=setting ? strtol(setting,&end,10) : 600000;
    if(!setting || end==setting || *end || ms<100 || ms>3600000)ms=600000;
    return monotonicTime()+(double)ms/1000;
}
static void pipeTransfer(int fd,void *buffer,size_t length,BOOL writing,double deadline) {
    size_t done=0;
    while(done<length) {
        double remaining=deadline-monotonicTime();
        if(remaining<=0)raiseError(@"Engine worker timed out");
        int timeout=(int)ceil(remaining*1000);struct pollfd p={.fd=fd,.events=writing?POLLOUT:POLLIN};
        int ready=poll(&p,1,timeout);
        if(ready<0 && errno==EINTR)continue;
        if(ready==0)raiseError(@"Engine worker timed out");
        if(ready<0 || (p.revents&POLLNVAL))raiseError(@"Engine worker pipe failed");
        ssize_t n=writing?write(fd,(uint8_t *)buffer+done,length-done):read(fd,(uint8_t *)buffer+done,length-done);
        if(n<0 && (errno==EINTR || errno==EAGAIN))continue;
        if(n<=0)raiseError(@"Engine worker stopped unexpectedly");
        done+=(size_t)n;
    }
}

@interface Worker : NSObject
@property(nonatomic, strong) NSLock *lock;
@property(nonatomic, strong) NSDictionary *speakers;
@property(nonatomic, strong) NSArray *speakerOrder;
@property(nonatomic) int inputFD;
@property(nonatomic) int outputFD;
@property(nonatomic) pid_t pid;
@property(nonatomic) NSTimeInterval used;
- (instancetype)initWithPack:(NSString *)pack lib:(NSString *)lib verbose:(BOOL)verbose;
- (NSData *)synthesize:(NSDictionary *)job;
- (void)stop;
@end
static BOOL validSpeakerOrder(id order,id speakers) {
    if(![order isKindOfClass:[NSArray class]] || ![speakers isKindOfClass:[NSDictionary class]] || [order count]!=[speakers count])return NO;
    NSMutableSet *seen=[NSMutableSet set];for(id name in order){
        if(![name isKindOfClass:[NSString class]] || ![speakers[name] isKindOfClass:[NSString class]] || [seen containsObject:name])return NO;
        [seen addObject:name];
    }return YES;
}
@implementation Worker
- (instancetype)initWithPack:(NSString *)pack lib:(NSString *)lib verbose:(BOOL)verbose {
    if(!(self=[super init])) return nil; _lock=[NSLock new]; _pid=-1; _inputFD=-1; _outputFD=-1; _used=[NSDate timeIntervalSinceReferenceDate];
    int in[2],out[2]; if(pipe(in)) raiseError(@"Cannot create worker pipe");
    if(pipe(out)) {close(in[0]);close(in[1]);raiseError(@"Cannot create worker pipe");}
    for(int i=0;i<2;++i) { fcntl(in[i],F_SETFD,FD_CLOEXEC); fcntl(out[i],F_SETFD,FD_CLOEXEC); }
    posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions,in[0],0); posix_spawn_file_actions_adddup2(&actions,out[1],1);
    posix_spawn_file_actions_addinherit_np(&actions,2);
    posix_spawn_file_actions_addclose(&actions,in[1]); posix_spawn_file_actions_addclose(&actions,out[0]);
    posix_spawnattr_t attributes;posix_spawnattr_init(&attributes);posix_spawnattr_setflags(&attributes,POSIX_SPAWN_CLOEXEC_DEFAULT);
    NSMutableArray *args=[NSMutableArray arrayWithArray:@[engineExecutable,@"worker",@"--lib",lib,@"--pack",pack]];
    if(verbose) [args addObject:@"--verbose"];
    char **argv=calloc(args.count+1,sizeof *argv);
    if(!argv){posix_spawn_file_actions_destroy(&actions);posix_spawnattr_destroy(&attributes);
        close(in[0]);close(in[1]);close(out[0]);close(out[1]);raiseError(@"Cannot allocate worker arguments");}
    for(NSUInteger i=0;i<args.count;++i) argv[i]=(char *)[args[i] fileSystemRepresentation];
    int result=posix_spawn(&_pid,argv[0],&actions,&attributes,argv,environ); free(argv); posix_spawn_file_actions_destroy(&actions);posix_spawnattr_destroy(&attributes);
    close(in[0]);close(out[1]);
    if(result) {close(in[1]);close(out[0]);raiseError([NSString stringWithFormat:@"Cannot start worker: %s",strerror(result)]);}
    _inputFD=in[1];_outputFD=out[0];
    @try {
        if(fcntl(_inputFD,F_SETFL,O_NONBLOCK)<0 || fcntl(_outputFD,F_SETFL,O_NONBLOCK)<0)raiseError(@"Cannot configure worker pipes");
        NSDictionary *hello=[self readHeader:workerDeadline()];
        if(hello[@"error"])raiseError([hello[@"error"] description]);
        if(![hello[@"speakers"] isKindOfClass:[NSDictionary class]])raiseError(@"Worker returned invalid voice catalog");
        _speakers=hello[@"speakers"];
        _speakerOrder=validSpeakerOrder(hello[@"speaker_order"],_speakers)?hello[@"speaker_order"]:[[_speakers allKeys] sortedArrayUsingSelector:@selector(compare:)];
        return self;
    } @catch(NSException *error) { [self stop]; @throw error; }
}
- (NSDictionary *)readHeader:(double)deadline {
    NSMutableData *data=[NSMutableData data];
    for(;;) {uint8_t ch;pipeTransfer(_outputFD,&ch,1,NO,deadline);if(ch=='\n')break;
        if(data.length>=65536)raiseError(@"Engine worker header is too large");[data appendBytes:&ch length:1];}
    id result=parseJSON(data);
    if(![result isKindOfClass:[NSDictionary class]]) raiseError(@"Invalid engine response");return result;
}
- (NSData *)synthesize:(NSDictionary *)job {
    [_lock lock];
    BOOL validError=NO;
    @try {
        _used=[NSDate timeIntervalSinceReferenceDate];
        if(_pid<=0)raiseError(@"Engine worker is unavailable");
        double deadline=workerDeadline();NSData *data=jsonData(job);uint8_t newline='\n';
        pipeTransfer(_inputFD,(void *)data.bytes,data.length,YES,deadline);pipeTransfer(_inputFD,&newline,1,YES,deadline);
        NSDictionary *header=[self readHeader:deadline];
        if(header[@"error"]) {validError=![header[@"broken"] boolValue];raiseError([header[@"error"] description]);}
        NSNumber *length=header[@"bytes"]; if(![length isKindOfClass:[NSNumber class]] || length.unsignedLongLongValue>128*1024*1024 || length.unsignedLongLongValue%4) raiseError(@"Invalid audio length from worker");
        NSMutableData *audio=[NSMutableData dataWithLength:length.unsignedIntegerValue];
        pipeTransfer(_outputFD,audio.mutableBytes,audio.length,NO,deadline);
        return audio;
    } @catch(NSException *error) {if(!validError)[self stop];@throw error;}
      @finally { _used=[NSDate timeIntervalSinceReferenceDate]; [_lock unlock]; }
}
- (void)stop {
    if(_inputFD>=0){close(_inputFD);_inputFD=-1;}if(_outputFD>=0){close(_outputFD);_outputFD=-1;}
    pid_t pid=_pid;_pid=-1;if(pid<=0)return;
    int status=0;pid_t r;do {r=waitpid(pid,&status,WNOHANG);}while(r<0 && errno==EINTR);
    if(r!=0)return;
    kill(pid,SIGTERM);double deadline=monotonicTime()+0.25;
    do {do {r=waitpid(pid,&status,WNOHANG);}while(r<0 && errno==EINTR);
        if(r!=0)return;usleep(10000);}while(monotonicTime()<deadline);
    kill(pid,SIGKILL);while(waitpid(pid,&status,0)<0 && errno==EINTR){}
}
- (void)dealloc { [self stop]; }
@end

@interface Voices : NSObject
@property(nonatomic,strong) NSString *lib;
@property(nonatomic,strong) NSString *data;
@property(nonatomic,strong) NSDictionary<NSString *,NSString *> *packs;
@property(nonatomic,strong) NSMutableDictionary<NSString *,Worker *> *workers;
@property(nonatomic,strong) NSLock *lock;
@property(nonatomic,strong) NSLock *catalogLock;
@property(nonatomic,strong) NSArray<NSDictionary *> *cachedCatalog;
@property(nonatomic) BOOL verbose;
@property(nonatomic) BOOL directPack;
- (instancetype)initWithLib:(NSString *)lib data:(NSString *)data pack:(NSString *)pack verbose:(BOOL)verbose;
- (Worker *)worker:(NSString *)pack;
- (NSArray<NSDictionary *> *)catalog;
- (void)startReaper;
- (NSData *)synthesize:(NSDictionary *)job defaults:(NSDictionary *)defaults;
@end
@implementation Voices
- (instancetype)initWithLib:(NSString *)lib data:(NSString *)data pack:(NSString *)pack verbose:(BOOL)verbose {
    if(!(self=[super init])) return nil; _lib=lib; _data=data; _verbose=verbose; _directPack=(pack!=nil); _workers=[NSMutableDictionary dictionary];_lock=[NSLock new];_catalogLock=[NSLock new];
    if(![[NSFileManager defaultManager] isReadableFileAtPath:lib]) raiseError([NSString stringWithFormat:@"%@ is missing. Run gtts fetch.",lib]);
    NSMutableDictionary *packs=[NSMutableDictionary dictionary];
    if(pack) {NSString *name=[[pack lastPathComponent] stringByDeletingPathExtension];packs[name]=pack;}
    else {
        NSString *folder=[data stringByAppendingPathComponent:@"voices"];
        for(NSString *name in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:folder error:nil])
            if([name.pathExtension isEqualToString:@"zvoice"]) packs[name.stringByDeletingPathExtension]=[folder stringByAppendingPathComponent:name];
    }
    if(!packs.count) raiseError(@"No voice packs installed. Run gtts fetch."); _packs=packs; return self;
}
- (Worker *)worker:(NSString *)pack {
    [_lock lock];
    @try {
        NSString *path=_packs[pack];if(!path) raiseError([NSString stringWithFormat:@"No pack '%@'",pack]);
        Worker *w=_workers[pack];
        if(w && [w.lock tryLock]) {int status=0;pid_t pid=w.pid;
            if(pid>0 && waitpid(pid,&status,WNOHANG)==pid)w.pid=-1;
            BOOL stopped=w.pid<=0;[w.lock unlock];if(stopped){[_workers removeObjectForKey:pack];w=nil;}}
        if(!w) {w=[[Worker alloc] initWithPack:path lib:_lib verbose:_verbose];_workers[pack]=w;}return w;
    } @finally { [_lock unlock]; }
}
- (NSArray<NSDictionary *> *)catalog {
    [_catalogLock lock];
    @try {
    if(_cachedCatalog) return _cachedCatalog;
    NSMutableArray *out=[NSMutableArray array];
    NSString *cacheDir=[_data stringByAppendingPathComponent:@"speaker-cache"];ensureDirectory(cacheDir);
    NSString *engineHash=sha256File(_lib);
    for(NSString *pack in [[_packs allKeys] sortedArrayUsingSelector:@selector(compare:)]) {
        NSString *cache=[cacheDir stringByAppendingPathComponent:[NSString stringWithFormat:@"%@-%@.json",engineHash,sha256File(_packs[pack])]];
        NSDictionary *speakers=nil;NSArray *order=nil;NSData *saved=[NSData dataWithContentsOfFile:cache];
        if(saved) { @try { id object=parseJSON(saved);if([object isKindOfClass:[NSDictionary class]]) {
            id stored=object[@"speakers"],storedOrder=object[@"speaker_order"];
            if(validSpeakerOrder(storedOrder,stored) && [stored count]){speakers=stored;order=storedOrder;}
        }} @catch(NSException *ignored) {(void)ignored;} }
        if(!speakers) {Worker *worker=[self worker:pack];speakers=worker.speakers;order=worker.speakerOrder;
            atomicWrite(cache,jsonData(@{@"speakers":speakers,@"speaker_order":order}));}
        NSString *prefix=pack.length>=5 ? [pack substringToIndex:5] : pack;
        NSArray *pieces=[prefix componentsSeparatedByString:@"-"];
        NSString *lang=pieces.count>1 ? [NSString stringWithFormat:@"%@-%@",pieces[0],[pieces[1] uppercaseString]] : prefix;
        for(NSString *name in order)
            [out addObject:@{@"id":[NSString stringWithFormat:@"%@/%@",pack,name],@"name":name,@"language":lang,@"gender":speakers[name]}];
    } _cachedCatalog=[out copy];return _cachedCatalog;
    } @finally { [_catalogLock unlock]; }
}
- (void)startReaper {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0), ^{
        for(;;) { sleep(60); @autoreleasepool {
            NSMutableArray *retired=[NSMutableArray array];[self.lock lock];
            for(NSString *pack in [self.workers.allKeys copy]) {
                Worker *w=self.workers[pack];
                if([w.lock tryLock]) {
                    BOOL idle=[NSDate timeIntervalSinceReferenceDate]-w.used>600;
                    [w.lock unlock]; if(idle){[retired addObject:w];[self.workers removeObjectForKey:pack];}
                }
            }
            [self.lock unlock];[retired removeAllObjects];
        }}
    });
}
- (NSData *)synthesize:(NSDictionary *)job defaults:(NSDictionary *)defaults {
    if(![job isKindOfClass:[NSDictionary class]]) raiseError(@"Expected a JSON object");
    id text=job[@"text"];if(![text isKindOfClass:[NSString class]]) raiseError(@"text must be a string");
    id voice=job[@"voice"] ?: defaults[@"voice"]; id speaker=job[@"speaker"];
    if(speaker==[NSNull null])speaker=@"sfg";else if(!speaker)speaker=defaults[@"speaker"] ?: @"sfg";
    NSString *pack;
    if(!_directPack && voice && voice!=[NSNull null]) {
        if(![voice isKindOfClass:[NSString class]]) raiseError(@"voice must be a string");
        NSRange slash=[voice rangeOfString:@"/"];if(slash.location==NSNotFound) raiseError(@"voice must have the form pack/speaker");
        pack=[voice substringToIndex:slash.location]; speaker=[voice substringFromIndex:slash.location+1];
    } else {pack=_packs[@"en-us-x-multi"] ? @"en-us-x-multi" : [[[_packs allKeys] sortedArrayUsingSelector:@selector(compare:)] firstObject];}
    if(![speaker isKindOfClass:[NSString class]]) raiseError(@"speaker must be a string");
    Worker *worker=[self worker:pack];NSString *gender=worker.speakers[speaker];
    if(worker.speakers.count && !gender) raiseError([NSString stringWithFormat:@"No speaker '%@' in pack '%@'",speaker,pack]);
    id rate=job[@"rate"] ?: defaults[@"rate"] ?: @1, pitch=job[@"pitch"] ?: defaults[@"pitch"] ?: @1;
    if(![rate respondsToSelector:@selector(doubleValue)] || ![pitch respondsToSelector:@selector(doubleValue)] ||
       !isfinite([rate doubleValue]) || [rate doubleValue]<=0 || !isfinite([pitch doubleValue]) || [pitch doubleValue]<=0) raiseError(@"rate and pitch must be positive numbers");
    return [worker synthesize:@{@"text":text,@"speaker":speaker,@"gender":defaults[@"gender"] ?: gender ?: @"",@"rate":@([rate doubleValue]),@"pitch":@([pitch doubleValue])}];
}
@end

static void reply(int fd,int code,NSString *type,NSData *body) {
    const char *reason=code==200?"OK":code==400?"Bad Request":code==404?"Not Found":code==501?"Not Implemented":"Internal Server Error";
    NSString *header=[NSString stringWithFormat:@"HTTP/1.1 %d %s\r\nContent-Type: %@\r\nContent-Length: %lu\r\nConnection: close\r\n\r\n",code,reason,type,(unsigned long)body.length];
    const uint8_t *parts[]={ (const uint8_t *)header.UTF8String, body.bytes }; size_t lengths[]={strlen(header.UTF8String),body.length};
    for(int part=0;part<2;++part) {size_t offset=0;while(offset<lengths[part]) {ssize_t n=send(fd,parts[part]+offset,lengths[part]-offset,0);
        if(n<0 && errno==EINTR) continue;if(n<=0)return;offset+=(size_t)n;}}
}
static ssize_t requestRecv(int fd,void *buffer,size_t length,double deadline) {
    for(;;) {
        double remaining=deadline-monotonicTime();if(remaining<=0)raiseError(@"HTTP request timed out");
        struct pollfd p={.fd=fd,.events=POLLIN};int ready=poll(&p,1,(int)ceil(remaining*1000));
        if(ready<0 && errno==EINTR)continue;
        if(!ready)raiseError(@"HTTP request timed out");if(ready<0 || (p.revents&POLLNVAL))raiseError(@"HTTP connection failed");
        ssize_t n=recv(fd,buffer,length,0);if(n<0 && errno==EINTR)continue;return n;
    }
}
static NSDictionary *readRequest(int fd,double deadline) {
    NSMutableData *buffer=[NSMutableData data]; NSData *delimiter=[@"\r\n\r\n" dataUsingEncoding:NSUTF8StringEncoding]; NSRange end;
    while(1) {
        end=[buffer rangeOfData:delimiter options:0 range:NSMakeRange(0,buffer.length)]; if(end.location!=NSNotFound) break;
        if(buffer.length>65536) raiseError(@"Request headers are too large"); uint8_t chunk[8192];ssize_t n=requestRecv(fd,chunk,sizeof chunk,deadline);
        if(n<0 && errno==EINTR)continue;if(n<=0)raiseError(@"Incomplete HTTP headers");[buffer appendBytes:chunk length:(NSUInteger)n];
    }
    if(end.location>65536)raiseError(@"Request headers are too large");
    NSString *header=[[NSString alloc] initWithData:[buffer subdataWithRange:NSMakeRange(0,end.location)] encoding:NSUTF8StringEncoding];
    if(!header)raiseError(@"Invalid HTTP headers");NSArray *lines=[header componentsSeparatedByString:@"\r\n"];
    NSArray *first=[lines[0] componentsSeparatedByString:@" "];if(first.count<2)raiseError(@"Invalid HTTP request");
    NSUInteger contentLength=0;BOOL lengthSeen=NO;
    for(NSUInteger i=1;i<lines.count;++i) {
        NSString *line=lines[i];NSRange colon=[line rangeOfString:@":"];if(colon.location==NSNotFound)continue;
        NSString *key=[[line substringToIndex:colon.location] lowercaseString];NSString *value=[[line substringFromIndex:colon.location+1] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        if([key isEqualToString:@"transfer-encoding"])raiseError(@"Transfer encoding is not supported");
        if([key isEqualToString:@"content-length"]) {NSScanner *scanner=[NSScanner scannerWithString:value];unsigned long long n=0;
            if(lengthSeen || ![scanner scanUnsignedLongLong:&n] || !scanner.isAtEnd || n>1024*1024)raiseError(@"Invalid Content-Length (maximum 1 MiB)");contentLength=(NSUInteger)n;lengthSeen=YES;}
    }
    NSUInteger bodyStart=end.location+delimiter.length;
    while(buffer.length-bodyStart<contentLength) {uint8_t chunk[16384];NSUInteger remain=contentLength-(buffer.length-bodyStart);ssize_t n=requestRecv(fd,chunk,MIN(sizeof chunk,remain),deadline);
        if(n<0&&errno==EINTR)continue;if(n<=0)raiseError(@"Incomplete HTTP body");[buffer appendBytes:chunk length:(NSUInteger)n];}
    return @{@"method":first[0],@"path":first[1],@"body":[buffer subdataWithRange:NSMakeRange(bodyStart,contentLength)]};
}
static unsigned activeClients;
typedef struct {int fd; double deadline; void *voices; void *defaults;} Client;
static void *handleClient(void *opaque) {
    @autoreleasepool {
        Client *client=opaque;int fd=client->fd;double deadline=client->deadline;Voices *voices=(__bridge_transfer Voices *)client->voices;
        NSDictionary *defaults=(__bridge_transfer NSDictionary *)client->defaults;free(client);
        struct timeval timeout={.tv_sec=10,.tv_usec=0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);
        @try {
            NSDictionary *request=readRequest(fd,deadline);NSString *method=request[@"method"],*path=request[@"path"];
            if([method isEqualToString:@"GET"] && [path isEqualToString:@"/health"]) reply(fd,200,@"application/json",[@"{\"ok\": true}" dataUsingEncoding:NSUTF8StringEncoding]);
            else if([method isEqualToString:@"GET"] && [path isEqualToString:@"/voices"]) reply(fd,200,@"application/json",jsonData([voices catalog]));
            else if([method isEqualToString:@"POST"] && [path isEqualToString:@"/speak"]) {
                id job=parseJSON(request[@"body"]);NSMutableDictionary *httpDefaults=[defaults mutableCopy];[httpDefaults removeObjectForKey:@"voice"];[httpDefaults removeObjectForKey:@"speaker"];
                NSData *audio=[voices synthesize:job defaults:httpDefaults];
                if([job[@"format"] isEqual:@"f32"])reply(fd,200,@"audio/L32; rate=24000",audio);
                else reply(fd,200,@"audio/wav",wavData(audio));
            } else if([method isEqualToString:@"GET"] || [method isEqualToString:@"POST"])reply(fd,404,@"text/plain",[NSData data]);
            else reply(fd,501,@"text/plain",[NSData data]);
        } @catch(NSException *error) { reply(fd,400,@"application/json",jsonData(@{@"error":error.reason ?: @"Request failed"})); }
        @finally {close(fd);__atomic_sub_fetch(&activeClients,1,__ATOMIC_RELEASE);}
    } return NULL;
}
static void serve(Voices *voices,NSDictionary *defaults,int port,NSString *host) {
    NSArray *catalog=[voices catalog];[voices startReaper];int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0)raiseError(@"Cannot create server socket");
    fcntl(fd,F_SETFD,FD_CLOEXEC);
    int yes=1;setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof yes);
    struct sockaddr_in addr={.sin_len=sizeof addr,.sin_family=AF_INET,.sin_port=htons((uint16_t)port)};
    if(inet_pton(AF_INET,host.UTF8String,&addr.sin_addr)!=1){close(fd);raiseError(@"--host must be an IPv4 address");}
    if(bind(fd,(struct sockaddr *)&addr,sizeof addr) || listen(fd,16)) {close(fd);raiseError([NSString stringWithFormat:@"Cannot listen on %@:%d: %s",host,port,strerror(errno)]);}
    fprintf(stderr,"%lu voices from %lu packs. Listening on %s:%d\n",(unsigned long)catalog.count,(unsigned long)voices.packs.count,host.UTF8String,port);
    for(;;) {
        int connection=accept(fd,NULL,NULL);if(connection<0){if(errno==EINTR)continue;close(fd);raiseError(@"Server accept failed");}
        fcntl(connection,F_SETFD,FD_CLOEXEC);
        if(__atomic_fetch_add(&activeClients,1,__ATOMIC_ACQ_REL)>=16) {
            __atomic_sub_fetch(&activeClients,1,__ATOMIC_RELEASE);
            const char busy[]="HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            shutdown(connection,SHUT_RD);send(connection,busy,sizeof busy-1,MSG_DONTWAIT);
            shutdown(connection,SHUT_WR);close(connection);continue;
        }
        Client *client=malloc(sizeof *client);if(!client){close(connection);__atomic_sub_fetch(&activeClients,1,__ATOMIC_RELEASE);continue;}
        *client=(Client){connection,monotonicTime()+10,(__bridge_retained void *)voices,(__bridge_retained void *)defaults};pthread_t thread;
        if(pthread_create(&thread,NULL,handleClient,client)) {CFRelease(client->voices);CFRelease(client->defaults);free(client);close(connection);__atomic_sub_fetch(&activeClients,1,__ATOMIC_RELEASE);continue;}
        pthread_detach(thread);
    }
}
static NSString *readStdin(void) {
    NSMutableData *data=[NSMutableData data]; uint8_t buffer[16384];size_t n;
    while((n=fread(buffer,1,sizeof buffer,stdin))) {if(data.length+n>16*1024*1024)raiseError(@"Text exceeds 16 MiB");[data appendBytes:buffer length:n];}
    if(ferror(stdin))raiseError(@"Cannot read stdin"); NSString *text=[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    if(!text)raiseError(@"Text is not UTF-8");return text;
}
static void printJSON(id object) {NSData *data=jsonData(object);fwrite(data.bytes,1,data.length,stdout);fputc('\n',stdout);fflush(stdout);}
static NSString *findEngine(void) {
    char name[PATH_MAX],resolved[PATH_MAX];uint32_t size=sizeof name;
    if(_NSGetExecutablePath(name,&size) || !realpath(name,resolved))raiseError(@"Cannot locate gtts executable");
    NSString *dir=[[NSString stringWithUTF8String:resolved] stringByDeletingLastPathComponent];
    NSArray *paths=@[[dir stringByAppendingPathComponent:@"gtts-engine"],
        [[dir stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"libexec/google-tts-local/gtts-engine"]];
    for(NSString *path in paths)if([[NSFileManager defaultManager] isExecutableFileAtPath:path])return path;
    raiseError(@"Cannot locate gtts-engine; reinstall gtts.");return nil;
}
static void usage(NSString *command) {
    if(!command) {
        printf("Usage: gtts [global options] COMMAND [options]\n\n"
               "Commands:\n  fetch    Download and verify the engine, runtimes, and voices\n"
               "  voices   List installed voices\n  speak    Synthesize text to a WAV file\n"
               "  batch    Synthesize one JSON job per input line\n  serve    Run the HTTP service\n"
               "  worker   Run the internal JSON/binary worker protocol\n"
               "  inspect  Inspect the guest ELF objects\n  init     Initialize the guest ELF objects\n\n"
               "Use gtts COMMAND --help for command-specific options.\n"
               "Global: --data-dir DIR (overrides GTTS_DATA), --version, --help\n"
               "Default data: ~/Library/Application Support/google-tts-local\n");return;
    }
    if([command isEqualToString:@"fetch"])printf("Usage: gtts fetch [--lang LANG ...] [--data-dir DIR]\nDownload the verified engine, voices, and runtimes, then convert the libraries to signed Mach-O.\nLanguages: en-us, es-es, fr-fr, hi-in, ja-jp, nl-nl (default: all).\n");
    else if([command isEqualToString:@"voices"])printf("Usage: gtts voices [--data-dir DIR] [--lib FILE] [--pack FILE] [--verbose]\nList voice IDs, languages, and genders. Run gtts fetch first.\n");
    else if([command isEqualToString:@"speak"])printf("Usage: gtts speak --out FILE [--text TEXT] [voice options]\nRead UTF-8 text from stdin if --text is omitted and write a WAV file.\n");
    else if([command isEqualToString:@"batch"])printf("Usage: gtts batch [voice options]\nRead one JSON object per line with text and out; voice, speaker, rate, and pitch are optional.\n");
    else if([command isEqualToString:@"serve"])printf("Usage: gtts serve [--host IPv4] [--port PORT] [voice options]\nServe GET /health, GET /voices, and POST /speak (default 127.0.0.1:5591).\nPOST accepts text, voice, speaker, rate, pitch, and format f32 for raw float audio.\nHTTP voice and speaker come from the request, not CLI defaults.\n");
    else if([command isEqualToString:@"worker"])printf("Usage: gtts worker --pack FILE [--lib FILE] [--data-dir DIR] [--verbose]\nInternal protocol: JSON lines in; JSON headers and float32 audio out.\n");
    else printf("Usage: gtts %s [--lib FILE] [--data-dir DIR]\nInspect or initialize the guest engine without synthesis.\n",command.UTF8String);
    if([@[@"speak",@"batch",@"serve"] containsObject:command])
        printf("Voice options: --voice PACK/SPEAKER, --pack FILE, --speaker NAME, --gender GENDER,\n"
               "               --rate N, --pitch N, --lib FILE, --data-dir DIR, --verbose\n");
    printf("--data-dir overrides GTTS_DATA; default is ~/Library/Application Support/google-tts-local.\n");
}

int main(int argc,char **argv) {
    @autoreleasepool {
        signal(SIGPIPE,SIG_IGN);
        if(argc<2){usage(nil);return 2;}
        if(argc==2 && !strcmp(argv[1],"--version")){printf("gtts %s\n",GTTS_VERSION);return 0;}
        @try {
            NSString *command=nil;
            NSArray *commands=@[@"fetch",@"voices",@"speak",@"batch",@"serve",@"worker",@"inspect",@"init"];
            NSString *data=nil;BOOL help=NO;
            NSMutableDictionary *options=[@{@"speaker":@"sfg",@"rate":@1.0,@"pitch":@1.0,@"port":@5591} mutableCopy];
            NSMutableArray *langs=[NSMutableArray array];
            for(int i=1;i<argc;++i) {
                NSString *arg=[NSString stringWithUTF8String:argv[i]];
                if([arg isEqualToString:@"--help"] || [arg isEqualToString:@"-h"]){help=YES;continue;}
                if([commands containsObject:arg] && !command) {command=arg;continue;}
                if([arg isEqualToString:@"--verbose"]){options[@"verbose"]=@YES;continue;}
                NSString *value=nil;NSRange equals=[arg rangeOfString:@"="];
                if(equals.location!=NSNotFound && [arg hasPrefix:@"--"]) {value=[arg substringFromIndex:equals.location+1];arg=[arg substringToIndex:equals.location];}
                else {if(i+1>=argc)raiseError([NSString stringWithFormat:@"Missing value for %@",arg]);value=[NSString stringWithUTF8String:argv[++i]];}
                if([arg isEqualToString:@"--data-dir"])data=value;
                else if([arg isEqualToString:@"--lang"]) {if(![langs containsObject:value])[langs addObject:value];}
                else if([@[@"--lib",@"--voice",@"--pack",@"--speaker",@"--gender",@"--text",@"--out",@"--host"] containsObject:arg]) {options[[arg substringFromIndex:2]]=value;}
                else if([@[@"--rate",@"--pitch"] containsObject:arg]) {char *end;double n=strtod(value.UTF8String,&end);
                    if(end==value.UTF8String || *end || !isfinite(n) || n<=0)raiseError([NSString stringWithFormat:@"Invalid %@: %@",arg,value]);options[[arg substringFromIndex:2]]=@(n);}
                else if([arg isEqualToString:@"--port"]) {char *end;long n=strtol(value.UTF8String,&end,10);if(end==value.UTF8String||*end||n<1||n>65535)raiseError(@"Invalid port");options[@"port"]=@(n);}
                else raiseError([NSString stringWithFormat:@"Unknown option: %@",arg]);
            }
            if(help){usage(command);return 0;}if(!command){usage(nil);return 2;}
            if(!data && getenv("GTTS_DATA"))data=[NSString stringWithUTF8String:getenv("GTTS_DATA")];
            if(!data) {NSString *home=getenv("HOME")?[NSString stringWithUTF8String:getenv("HOME")]:NSHomeDirectory();
                if(!home.length)raiseError(@"HOME is not set; use --data-dir.");data=[home stringByAppendingPathComponent:@"Library/Application Support/google-tts-local"];}
            data=absolutePath(data);setenv("GTTS_DATA",data.fileSystemRepresentation,1);
            if([command isEqualToString:@"fetch"]) {
                ensureDirectory(data);NSString *temporary=[data stringByAppendingPathComponent:@"tmp"];ensureDirectory(temporary);
                setenv("TMPDIR",temporary.fileSystemRepresentation,1);fetch(data,langs.count?langs:languages());return 0;
            }
            NSString *lib=options[@"lib"] ? absolutePath(options[@"lib"]) : [data stringByAppendingPathComponent:@"libchrometts.so"];
            if(![[NSFileManager defaultManager] isReadableFileAtPath:lib])raiseError([NSString stringWithFormat:@"%@ is missing. Run gtts fetch.",lib]);
            requireRuntime(data);
            ensureDirectory(data);NSString *temporary=[data stringByAppendingPathComponent:@"tmp"];ensureDirectory(temporary);
            ensureDirectory([data stringByAppendingPathComponent:@"installed"]);setenv("TMPDIR",temporary.fileSystemRepresentation,1);
            engineExecutable=findEngine();
            if([@[@"worker",@"inspect",@"init"] containsObject:command]) {
                NSMutableArray *args=[NSMutableArray arrayWithArray:@[engineExecutable,command]];
                setenv("GTTS_NATIVE_LIB",lib.fileSystemRepresentation,1);
                if([command isEqualToString:@"worker"]) {if(!options[@"pack"])raiseError(@"worker requires --pack");[args addObjectsFromArray:@[@"--lib",lib,@"--pack",options[@"pack"]]];if(options[@"verbose"]) [args addObject:@"--verbose"];}
                char **execargs=calloc(args.count+1,sizeof *execargs);for(NSUInteger i=0;i<args.count;++i)execargs[i]=(char *)[args[i] fileSystemRepresentation];
                execv(execargs[0],execargs);free(execargs);raiseError(@"Cannot start engine");
            }
            Voices *voices=[[Voices alloc] initWithLib:lib data:data pack:options[@"pack"] verbose:[options[@"verbose"] boolValue]];
            if([command isEqualToString:@"voices"]) {for(NSDictionary *v in [voices catalog])printf("%-24s %-6s %s\n",[v[@"id"] UTF8String],[v[@"language"] UTF8String],[v[@"gender"] UTF8String]);return 0;}
            if([command isEqualToString:@"serve"]) {serve(voices,options,[options[@"port"] intValue],options[@"host"] ?: @"127.0.0.1");return 0;}
            if([command isEqualToString:@"batch"]) {
                char *line=NULL;size_t capacity=0;ssize_t len;
                @try {while((len=getline(&line,&capacity,stdin))>=0) {@autoreleasepool {
                    NSData *dataLine=[NSData dataWithBytes:line length:(NSUInteger)len];NSString *trim=[[[NSString alloc] initWithData:dataLine encoding:NSUTF8StringEncoding] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
                    if(!trim)raiseError(@"Batch input is not UTF-8");if(!trim.length)continue;id job=parseJSON(dataLine);if(![job isKindOfClass:[NSDictionary class]] || ![job[@"out"] isKindOfClass:[NSString class]])raiseError(@"batch requires a string out path");
                    NSData *audio=[voices synthesize:job defaults:options];atomicWrite(job[@"out"],wavData(audio));
                    printJSON(@{@"out":job[@"out"],@"seconds":@(round(((double)audio.length/4/24000)*100)/100)});
                }}} @finally {free(line);}return 0;
            }
            if(!options[@"out"])raiseError(@"speak needs --out");
            NSString *text=options[@"text"] ?: readStdin();NSData *audio=[voices synthesize:@{@"text":text} defaults:options];atomicWrite(options[@"out"],wavData(audio));
            printJSON(@{@"out":options[@"out"],@"seconds":@(round(((double)audio.length/4/24000)*100)/100)});return 0;
        } @catch(NSException *error) {fprintf(stderr,"error: %s\n",(error.reason ?: @"Unknown error").UTF8String);return 1;}
    }
}
