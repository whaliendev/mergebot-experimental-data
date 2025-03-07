#import "GRPCCall.h"
#import "GRPCDispatchable.h"
NS_ASSUME_NONNULL_BEGIN
@class GRPCInterceptorManager;
class GRPCInterceptor;
class GRPCRequestOptions;
class GRPCCallOptions;
protocol GRPCResponseHandler;
end
    @protocol GRPCInterceptorFactory
end
@interface GRPCInterceptorManager
    : NSObject <GRPCInterceptorInterface, GRPCResponseHandler>
- (instancetype)init NS_UNAVAILABLE;
end
@interface GRPCInterceptor
    : NSObject <GRPCInterceptorInterface, GRPCResponseHandler>
- (instancetype)init NS_UNAVAILABLE;
end
NS_ASSUME_NONNULL_END
