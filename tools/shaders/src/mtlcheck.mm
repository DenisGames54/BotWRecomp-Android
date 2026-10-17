// mtlcheck: compile .metal files with the Metal framework's own compiler (MTLDevice
// newLibraryWithSource), for machines without the offline Metal toolchain (xcrun metal needs
// `xcodebuild -downloadComponent MetalToolchain`). Same front end the runtime would use.
//
//   mtlcheck [-j N] [--std 3.1] file.metal...      prints "ok <file>" or "fail <file>\t<first error>"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <stdio.h>
#include <string.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    @autoreleasepool {
        int jobs = 4;
        MTLLanguageVersion ver = MTLLanguageVersion3_1;
        std::vector<std::string> files;
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "-j") && i + 1 < argc) jobs = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--std") && i + 1 < argc) {
                std::string v = argv[++i];
                ver = v == "2.4" ? MTLLanguageVersion2_4 : v == "3.0" ? MTLLanguageVersion3_0 : MTLLanguageVersion3_1;
            } else
                files.push_back(argv[i]);
        }
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) {
            fprintf(stderr, "no Metal device\n");
            return 1;
        }
        std::atomic<size_t> next{0};
        std::vector<std::string> results(files.size());
        auto worker = [&] {
            for (;;) {
                size_t i = next++;
                if (i >= files.size()) return;
                @autoreleasepool {
                    NSError* err = nil;
                    NSString* src = [NSString stringWithContentsOfFile:[NSString stringWithUTF8String:files[i].c_str()]
                                                              encoding:NSUTF8StringEncoding
                                                                 error:&err];
                    if (!src) {
                        results[i] = "fail " + files[i] + "\tcannot read";
                        continue;
                    }
                    MTLCompileOptions* opt = [MTLCompileOptions new];
                    opt.languageVersion = ver;
                    opt.mathMode = MTLMathModeSafe;
                    id<MTLLibrary> lib = [dev newLibraryWithSource:src options:opt error:&err];
                    if (lib) {
                        results[i] = "ok " + files[i];
                    } else {
                        std::string msg = err ? [[err localizedDescription] UTF8String] : "unknown";
                        // first "error:" line
                        size_t e = msg.find("error:");
                        std::string first = e == std::string::npos ? msg : msg.substr(e);
                        size_t nl = first.find('\n');
                        if (nl != std::string::npos) first = first.substr(0, nl);
                        results[i] = "fail " + files[i] + "\t" + first;
                    }
                }
            }
        };
        std::vector<std::thread> th;
        for (int k = 0; k < jobs; k++) th.emplace_back(worker);
        for (auto& t : th) t.join();
        for (auto& r : results) printf("%s\n", r.c_str());
    }
    return 0;
}
