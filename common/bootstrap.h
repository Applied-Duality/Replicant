// Copyright (c) 2012, Robert Escriva
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//     * Redistributions of source code must retain the above copyright notice,
//       this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * Neither the name of Replicant nor the names of its contributors may be
//       used to endorse or promote products derived from this software without
//       specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#ifndef replicant_common_bootstrap_h_
#define replicant_common_bootstrap_h_

// C++
#include <iostream>

// po6
#include <po6/net/hostname.h>

// Replicant
#include "common/configuration.h"

namespace replicant
{

enum bootstrap_returncode
{
    BOOTSTRAP_SUCCESS,
    BOOTSTRAP_TIMEOUT,
    BOOTSTRAP_COMM_FAIL,
    BOOTSTRAP_SEE_ERRNO,
    BOOTSTRAP_CORRUPT_INFORM,
    BOOTSTRAP_NOT_CLUSTER_MEMBER,
    BOOTSTRAP_GARBAGE
};

bootstrap_returncode
bootstrap(const po6::net::hostname& hn, configuration* config);

bootstrap_returncode
bootstrap_identity(const po6::net::hostname& hn, chain_node* cn);

bootstrap_returncode
bootstrap(const po6::net::hostname* hns, size_t hns_sz,
          configuration* config);

bool
bootstrap_parse_hosts(const char* connection_string,
                      std::vector<po6::net::hostname>* hosts);

std::string
bootstrap_hosts_to_string(const po6::net::hostname* hns, size_t hns_sz);

std::ostream&
operator << (std::ostream& lhs, const bootstrap_returncode& rhs);

} // namespace replicant

#endif // replicant_common_bootstrap_h_
