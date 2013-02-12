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

// Replicant
#include "common/chain_node.h"
#include "packing.h"

using replicant::chain_node;

chain_node :: chain_node()
    : token()
    , address()
{
}

chain_node :: ~chain_node() throw ()
{
}

bool
replicant :: operator < (const chain_node& lhs, const chain_node& rhs)
{
    if (lhs.token == rhs.token)
    {
        return lhs.address < rhs.address;
    }

    return lhs.token < rhs.token;
}

bool
replicant :: operator == (const chain_node& lhs, const chain_node& rhs)
{
    return lhs.token == rhs.token &&
           lhs.address == rhs.address;
}

std::ostream&
replicant :: operator << (std::ostream& lhs, const chain_node& rhs)
{
    return lhs << "chain_node(bind_to=" << rhs.address
               << ", token=" << rhs.token << ")";
}

e::buffer::packer
replicant :: operator << (e::buffer::packer lhs, const chain_node& rhs)
{
    return lhs << rhs.token << rhs.address;
}

e::unpacker
replicant :: operator >> (e::unpacker lhs, chain_node& rhs)
{
    return lhs >> rhs.token >> rhs.address;
}

size_t
replicant :: pack_size(const chain_node& rhs)
{
    return sizeof(uint64_t) + pack_size(rhs.address);
}
