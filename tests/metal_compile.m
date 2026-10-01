/* Compiles .metal files with the Metal framework's runtime compiler (newLibraryWithSource), as
 * the game's back end does, and prints the failures.  usage: build/metal_compile <files...> */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>

int main(int argc, char** argv)
{
    @autoreleasepool
    {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        MTLCompileOptions* o = [MTLCompileOptions new];
        o.languageVersion = MTLLanguageVersion3_0;
        int bad = 0, shown = 0;
        for (int i = 1; i < argc; ++i)
        {
            NSString* src = [NSString stringWithContentsOfFile:@(argv[i]) encoding:NSUTF8StringEncoding error:nil];
            NSError* err = nil;
            id<MTLLibrary> lib = [dev newLibraryWithSource:src options:o error:&err];
            if (!lib)
            {
                bad++;
                if (shown++ < 8)
                {
                    NSString* m = err.localizedDescription;
                    NSRange r = [m rangeOfString:@"error:"];
                    printf("== %s\n%.600s\n", argv[i], (r.location != NSNotFound ? [m substringFromIndex:r.location] : m).UTF8String);
                }
            }
        }
        printf("metal: %d of %d compile\n", argc - 1 - bad, argc - 1);
        return bad != 0;
    }
}
