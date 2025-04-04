#import <Carbon/Carbon.h>
#include <unistd.h>
void get_my_path(char *s, size_t maxLen)
{
    CFBundleRef mainBundle = CFBundleGetMainBundle();
    CFURLRef executableURL = CFBundleCopyExecutableURL(mainBundle);
    CFStringRef executablePathString = CFURLCopyFileSystemPath(executableURL, kCFURLPOSIXPathStyle);
    CFRelease(executableURL);
<<<<<<< HEAD
    CFStringGetFileSystemRepresentation(executablePathString, s, maxLen);
    CFRelease(executablePathString);
||||||| 1a65d5b35
<<<<<<< HEAD
    CFStringGetFileSystemRepresentation(executablePathString, s, maxLen);
    CFRelease(executablePathString);
||||||| 1a65d5b35
    CFStringGetCString(bundlePathString, s, maxLen, kCFStringEncodingASCII);
    CFRelease(bundlePathString);
=======
    CFStringGetCString(executablePathString, s, maxLen, kCFStringEncodingASCII);
    CFRelease(executablePathString);
>>>>>>> 6c555ea3
=======
    CFStringGetCString(executablePathString, s, maxLen, kCFStringEncodingASCII);
    CFRelease(executablePathString);
>>>>>>> 6c555ea3
}
