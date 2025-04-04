#import <Carbon/Carbon.h>
#include <unistd.h>
void get_my_path(char s[PATH_MAX])
{
    CFBundleRef mainBundle = CFBundleGetMainBundle();
    CFURLRef executableURL = CFBundleCopyExecutableURL(mainBundle);
    CFStringRef executablePathString = CFURLCopyFileSystemPath(executableURL, kCFURLPOSIXPathStyle);
    CFRelease(executableURL);
<<<<<<< HEAD
    CFStringGetFileSystemRepresentation(executablePathString, s, PATH_MAX-1);
    CFRelease(executablePathString);
||||||| 1a65d5b35
    CFStringGetCString(bundlePathString, s, PATH_MAX - 1, kCFStringEncodingASCII);
    CFRelease(bundlePathString);
=======
    CFStringGetCString(executablePathString, s, PATH_MAX-1, kCFStringEncodingASCII);
    CFRelease(executablePathString);
>>>>>>> 6c555ea3
 char *x;
    x = strrchr(s, '/');
    if(x) x[1] = 0;
}
