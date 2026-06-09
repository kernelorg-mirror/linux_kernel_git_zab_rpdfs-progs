/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RPDFS_DEVD_RLOCK_H
#define RPDFS_DEVD_RLOCK_H

int rlock_request(struct sockaddr_in *addr, struct rpdfs_rlock_key *key, u8 mode);
int rlock_confirm(struct sockaddr_in *addr, struct rpdfs_rlock_key *key, u8 mode);
int rlock_release(struct sockaddr_in *addr, struct rpdfs_rlock_key *key, u8 mode);

int rlock_init(void);
void rlock_exit(void);

#endif
