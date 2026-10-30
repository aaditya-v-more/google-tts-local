#import <Foundation/Foundation.h>
#include "speech.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static BOOL sendJSON(FILE *out, NSDictionary *object) {
    NSData *data = [NSJSONSerialization dataWithJSONObject:object options:0 error:nil];
    return data && fwrite(data.bytes, 1, data.length, out) == data.length && fputc('\n', out) != EOF;
}
// Each finished segment goes out as {"bytes":N,"more":true} followed by N bytes of
// float samples; the reply ends with a plain {"bytes":0} (or an error).
static BOOL deliveryFailed;
static int sendAudio(void *context, const float *samples, size_t count) {
    FILE *out = context;
    if (!count) return 0;
    if (sendJSON(out, @{ @"bytes": @(count * sizeof(float)), @"more": @YES }) &&
        fwrite(samples, sizeof(float), count, out) == count && !fflush(out)) return 0;
    deliveryFailed = YES; return -1;
}
static BOOL validString(id value) {
    return [value isKindOfClass:[NSString class]] && [value lengthOfBytesUsingEncoding:NSUTF8StringEncoding] == strlen([value UTF8String]);
}
int worker_run(Guest *guest, int protocol_fd, const char *pack, int verbose) {
    @autoreleasepool {
        signal(SIGPIPE, SIG_IGN);
        FILE *out = fdopen(protocol_fd, "wb"); if (!out) return 1;
        guest->verbose = verbose;
        SpeechEngine engine;
        if (speech_open(&engine, guest, pack)) {
            fprintf(stderr, "error: %s\n", engine.error);
            sendJSON(out, @{ @"error": [NSString stringWithUTF8String:engine.error] }); fflush(out);
            speech_close(&engine); fclose(out); return 1;
        }
        NSMutableDictionary *speakers = [NSMutableDictionary dictionary];
        NSMutableArray *speakerOrder=[NSMutableArray array];
        for (size_t i = 0; i < guest->speaker_count; ++i) {
            NSString *name=[NSString stringWithUTF8String:guest->speakers[i].name];[speakerOrder addObject:name];
            speakers[name] = [NSString stringWithUTF8String:guest->speakers[i].gender];
        }
        if (!sendJSON(out, @{ @"speakers": speakers, @"speaker_order":speakerOrder }) || fflush(out)) { speech_close(&engine); fclose(out); return 1; }
        char *line = NULL; size_t capacity = 0; ssize_t length;
        while ((length = getline(&line, &capacity, stdin)) >= 0) {
            @autoreleasepool {
                NSData *data = [NSData dataWithBytes:line length:(NSUInteger)length];
                id job = [NSJSONSerialization JSONObjectWithData:data options:NSJSONReadingFragmentsAllowed error:nil];
                NSString *error = nil; size_t count = 0;
                if (![job isKindOfClass:[NSDictionary class]]) error = @"Expected a JSON object";
                else if (!validString(job[@"text"]) || !validString(job[@"speaker"])) error = @"text and speaker must be strings without NUL characters";
                else if (job[@"gender"] && !validString(job[@"gender"])) error = @"gender must be a string";
                else if (job[@"rate"] && ![job[@"rate"] respondsToSelector:@selector(doubleValue)]) error = @"rate must be a number";
                else if (job[@"pitch"] && ![job[@"pitch"] respondsToSelector:@selector(doubleValue)]) error = @"pitch must be a number";
                else {
                    double rate = job[@"rate"] ? [job[@"rate"] doubleValue] : 1.0;
                    double pitch = job[@"pitch"] ? [job[@"pitch"] doubleValue] : 1.0;
                    if (speech_synth_stream(&engine, [job[@"text"] UTF8String], [job[@"speaker"] UTF8String],
                                            [job[@"gender"] UTF8String], rate, pitch, sendAudio, out, &count))
                        error = [NSString stringWithUTF8String:engine.error] ?: @"Synthesis failed";
                }
                if (deliveryFailed) break; // the reader has gone away
                BOOL ok;
                if (error) ok = sendJSON(out, engine.broken ? @{ @"error": error, @"broken": @YES } : @{ @"error": error });
                else ok = sendJSON(out, @{ @"bytes": @0 });
                if (!ok || fflush(out) || engine.broken) break;
            }
        }
        free(line); int broken=engine.broken; speech_close(&engine); fclose(out); return broken ? 1 : 0;
    }
}
