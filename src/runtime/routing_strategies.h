/**
 * @file routing_strategies.h
 * Declares strategies that MSUs can use for routing to endpoints
 */

#ifndef ROUTING_STRATEGIES_H_
#define ROUTING_STRATEGIES_H_

#include "msu_type.h"

/** The defualt routing strategy, using the key of the MSU message
 * to route to a pre-defined endpoint.
 * This function can be used as-is as an entry in the msu_type struct.
 */
int default_routing(struct msu_type *type, struct local_msu *sender,
                    struct msu_msg *msg, struct msu_endpoint *output);

/** Chooses the local MSU with the shortest queue.
 * This function can be used as-is as an entry in the msu_type struct
 */
int shortest_queue_route(struct msu_type *type, struct local_msu *sender,
                         struct msu_msg *msg, struct msu_endpoint *output);

/** Chooses the MSU with the given ID.
 * This function must be wrapped in another function to choose the
 * appropriate ID if it is to be used in the msu_type struct.
 */
int route_to_id(struct msu_type *type, struct local_msu *sender,
                int msu_id, struct msu_endpoint *output);

/** Routes an MSU message to the runtime on which the message originated.
 *
 * FIXME: If the message's provinance gets too long, the origin runtime
 * will be lost and this function will break!
 *
 * This function can be used as-is as an entry in the msu_type struct.
 */
int route_to_origin_runtime(struct msu_type *type, struct local_msu *sender,
                            struct msu_msg *msg, struct msu_endpoint *output);


#endif
