/*
 *
 * Copyright 2019 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */
/**
 * \file GRPCInterceptor.h
 * API for interceptors implementation. This feature is currently EXPERIMENTAL
and is subject to
 * breaking changes without prior notice.
 *
 * The interceptors in the gRPC system forms a chain. When a call is made by the
user, each
 * interceptor on the chain has chances to react to events of the call and make
necessary
 * modifications to the call's parameters, data, metadata, or flow.
 *
 * \verbatim
                                     -----------
                                    | GRPCCall2 |
                                     -----------
                                          |
                                          |
                             --------------------------
                            | GRPCInterceptorManager 1 |
                             --------------------------
                            | GRPCInterceptor 1        |
                             --------------------------
                                          |
                                         ...
                                          |
                             --------------------------
                            | GRPCInterceptorManager N |
                             --------------------------
                            | GRPCInterceptor N        |
                             --------------------------
                                          |
                                          |
                                 ------------------
                                | GRPCCallInternal |
                                 ------------------
   \endverbatim
 *
 * The chain of interceptors is initialized when the corresponding GRPCCall2
object or proto call
 * object (GRPCUnaryProtoCall and GRPCStreamingProtoCall) is initialized. The
initialization of the
 * chain is controlled by the property interceptorFactories in the callOptions
parameter of the
 * corresponding call object. Property interceptorFactories is an array of
 * id<GRPCInterceptorFactory> objects provided by the user. When a call object
is initialized, each
 * interceptor factory generates an interceptor object for the call. gRPC
internally links the
 * interceptors with each other and with the actual call object. The order of
the interceptors in
 * the chain is exactly the same as the order of factory objects in
interceptorFactories property.
 * All requests (start, write, finish, cancel, receive next) initiated by the
user will be processed
 * in the order of interceptors, and all responses (initial metadata, data,
trailing metadata, write
 * data done) are processed in the reverse order.
 *
 * Each interceptor in the interceptor chain should behave as a user of the next
interceptor, and at
 * the same time behave as a call to the previous interceptor. Therefore
interceptor implementations
 * must follow the state transition of gRPC calls and must also forward events
that are consistent
 * with the current state of the next/previous interceptor. They should also
make sure that the
 * events they forwarded to the next and previous interceptors will, in the end,
make the neighbour
 * interceptor terminate correctly and reaches "finished" state. The diagram
below shows the state
 * transitions. Any event not appearing on the diagram means the event is not
permitted for that
 * particular state.
 *
<<<<<<< HEAD
 * \verbatim
                                        writeData
                                    receiveNextMessages
                                 didReceiveInitialMetadata
                                      didReceiveData
                                       didWriteData receiveNextmessages
             writeData  -----             -----                 ----
didReceiveInitialMetadata receiveNextMessages |     |           |     | |    |
didReceiveData |     V           |     V               |    V didWriteData
                 -------------  start   ---------   finish    ------------
                | initialized | -----> | started | --------> | half-close |
                 -------------          ---------             ------------
                       |                     |                      |
                       |                     | didClose             | didClose
                       |cancel               | cancel               | cancel
                       |                     V                      |
                       |                 ----------                 |
                        --------------> | finished | <--------------
                                         ----------
                                          |      ^ writeData
                                          |      | finish
                                           ------  cancel
                                                   receiveNextMessages
   \endverbatim
 *
 * An interceptor must forward responses to its previous interceptor in the
order of initial
 * metadata, message(s), and trailing metadata. Forwarding responses out of this
order (e.g.
 * forwarding a message before initial metadata) is not allowed.
|||||||
 *                                      writeData
 *                                  receiveNextMessages
 *                               didReceiveInitialMetadata
 *                                    didReceiveData
 *                                     didWriteData receiveNextmessages
 *           writeData  -----             -----                 ----
didReceiveInitialMetadata
 * receiveNextMessages |     |           |     |               |    |
didReceiveData
 *                     |     V           |     V               |    V
didWriteData
 *               -------------  start   ---------   finish    ------------
 *              | initialized | -----> | started | --------> | half-close |
 *               -------------          ---------             ------------
 *                     |                     |                      |
 *                     |                     | didClose             | didClose
 *                     |cancel               | cancel               | cancel
 *                     |                     V                      |
 *                     |                 ----------                 |
 *                      --------------> | finished | <--------------
 *                                       ----------
 *                                        |      ^ writeData
 *                                        |      | finish
 *                                         ------  cancel
 *                                                 receiveNextMessages
=======
 * \verbatim
                                        writeData
                                    receiveNextMessages
                                 didReceiveInitialMetadata
                                      didReceiveData
                                       didWriteData receiveNextmessages
             writeData  -----             -----                 ----
didReceiveInitialMetadata receiveNextMessages |     |           |     | |    |
didReceiveData |     V           |     V               |    V didWriteData
                 -------------  start   ---------   finish    ------------
                | initialized | -----> | started | --------> | half-close |
                 -------------          ---------             ------------
                       |                     |                      |
                       |                     | didClose             | didClose
                       |cancel               | cancel               | cancel
                       |                     V                      |
                       |                 ----------                 |
                        --------------> | finished | <--------------
                                         ----------
                                          |      ^ writeData
                                          |      | finish
                                           ------  cancel
                                                   receiveNextMessages
   \endverbatim
>>>>>>> bca2a30e97f46e7eaec83da0e13c9313aca1ce53
 *
 * Events of requests and responses are dispatched to interceptor objects using
the interceptor's
 * dispatch queue. The dispatch queue should be serial queue to make sure the
events are processed
 * in order. Interceptor implementations must derive from GRPCInterceptor class.
The class makes
 * some basic implementation of all methods responding to an event of a call. If
an interceptor does
 * not care about a particular event, it can use the basic implementation of the
GRPCInterceptor
 * class, which simply forward the event to the next or previous interceptor in
the chain.
 *
 * The interceptor object should be unique for each call since the call context
is not passed to the
 * interceptor object in a call event. However, the interceptors can be
implemented to share states
 * by receiving state sharing object from the factory upon construction.
 */

#import "GRPCCall.h"
#import "GRPCDispatchable.h"

NS_ASSUME_NONNULL_BEGIN

@class GRPCInterceptorManager;
class GRPCInterceptor;
class GRPCRequestOptions;
class GRPCCallOptions;
protocol GRPCResponseHandler;

end

    /**
     * An interceptor factory object is used to create interceptor object for
     * the call at the call start time.
     */
    @protocol GRPCInterceptorFactory

/**
 * Create an interceptor object. gRPC uses the returned object as the
 * interceptor for the current call
 */
end

/**
 * GRPCInterceptorManager is a helper class to forward messages between the
 * interceptors. The interceptor manager object retains reference to the next
 * and previous interceptor object in the interceptor chain, and forward
 * corresponding events to them.
 *
 * All methods except the initializer of the class can only be called on the
 * manager's dispatch queue. Since the manager's dispatch queue targets
 * corresponding interceptor's dispatch queue, it is also safe to call the
 * manager's methods in the corresponding interceptor instance's methods that
 * implement GRPCInterceptorInterface.
 *
 * When an interceptor is shutting down, it must invoke -shutDown method of its
 * corresponding manager so that references to other interceptors can be
 * released and proper clean-up is made.
 */
@interface GRPCInterceptorManager
    : NSObject <GRPCInterceptorInterface, GRPCResponseHandler>

- (instancetype)init NS_UNAVAILABLE;

end

/**
 * Base class for a gRPC interceptor. The implementation of the base class
 * provides default behavior of an interceptor, which is simply forward a
 * request/callback to the next/previous interceptor in the chain. The base
 * class implementation uses the same dispatch queue for both requests and
 * callbacks.
 *
 * An interceptor implementation should inherit from this base class and
 * initialize the base class with [super
 * initWithInterceptorManager:dispatchQueue:] for the default implementation to
 * function properly.
 */
@interface GRPCInterceptor
    : NSObject <GRPCInterceptorInterface, GRPCResponseHandler>

- (instancetype)init NS_UNAVAILABLE;
// Default implementation of GRPCInterceptorInterface

// Default implementation of GRPCResponeHandler

end

NS_ASSUME_NONNULL_END