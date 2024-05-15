#import "GRPCCall.h"
#import "GRPCDispatchable.h"
NS_ASSUME_NONNULL_BEGIN
@class GRPCInterceptorManager;
@class GRPCInterceptor;
@class GRPCRequestOptions;
@class GRPCCallOptions;
@protocol GRPCResponseHandler;
@protocol GRPCInterceptorInterface<NSObject, GRPCDispatchable>
- (void)startWithRequestOptions:(GRPCRequestOptions *)requestOptions
                    callOptions:(GRPCCallOptions *)callOptions;
- (void)writeData:(id)data;
- (void)finish;
- (void)cancel;
- (void)receiveNextMessages:(NSUInteger)numberOfMessages;
@end
@protocol GRPCInterceptorFactory
- (GRPCInterceptor *)createInterceptorWithManager:(GRPCInterceptorManager *)interceptorManager;
@end
@interface GRPCInterceptorManager : NSObject<GRPCInterceptorInterface, GRPCResponseHandler>
- (instancetype)init NS_UNAVAILABLE;
+ (instancetype) new NS_UNAVAILABLE;
- (nullable instancetype)initWithFactories:(nullable NSArray<id<GRPCInterceptorFactory>> *)factories
                       previousInterceptor:(nullable id<GRPCResponseHandler>)previousInterceptor
                               transportID:(GRPCTransportID)transportID;
- (void)shutDown;
- (void)startNextInterceptorWithRequest:(GRPCRequestOptions *)requestOptions
                            callOptions:(GRPCCallOptions *)callOptions;
- (void)writeNextInterceptorWithData:(id)data;
- (void)finishNextInterceptor;
- (void)cancelNextInterceptor;
- (void)receiveNextInterceptorMessages:(NSUInteger)numberOfMessages;
- (void)forwardPreviousInterceptorWithInitialMetadata:(nullable NSDictionary *)initialMetadata;
- (void)forwardPreviousInterceptorWithData:(nullable id)data;
- (void)forwardPreviousInterceptorCloseWithTrailingMetadata:
            (nullable NSDictionary *)trailingMetadata
                                                      error:(nullable NSError *)error;
- (void)forwardPreviousInterceptorDidWriteData;
@end
@interface GRPCInterceptor : NSObject<GRPCInterceptorInterface, GRPCResponseHandler>
- (instancetype)init NS_UNAVAILABLE;
+ (instancetype) new NS_UNAVAILABLE;
- (nullable instancetype)initWithInterceptorManager:(GRPCInterceptorManager *)interceptorManager
                                      dispatchQueue:(dispatch_queue_t)dispatchQueue;
- (void)startWithRequestOptions:(GRPCRequestOptions *)requestOptions
                    callOptions:(GRPCCallOptions *)callOptions;
- (void)writeData:(id)data;
- (void)finish;
- (void)cancel;
- (void)receiveNextMessages:(NSUInteger)numberOfMessages;
- (void)didReceiveInitialMetadata:(nullable NSDictionary *)initialMetadata;
- (void)didReceiveData:(id)data;
- (void)didCloseWithTrailingMetadata:(nullable NSDictionary *)trailingMetadata
                               error:(nullable NSError *)error;
- (void)didWriteData;
@end
NS_ASSUME_NONNULL_END
