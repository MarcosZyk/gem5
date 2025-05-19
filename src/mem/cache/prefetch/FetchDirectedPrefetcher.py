# Copyright (c) 2025 All-Hands
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

from m5.params import *
from m5.proxy import *

from m5.objects.QueuedPrefetcher import QueuedPrefetcher

class FetchDirectedPrefetcher(QueuedPrefetcher):
    type = 'FetchDirectedPrefetcher'
    cxx_class = 'gem5::prefetch::FetchDirected'
    cxx_header = 'mem/cache/prefetch/fetch_directed.hh'

    # PIQ (Prefetch Instruction Queue) size
    piq_size = Param.Unsigned(16, "Maximum size of the PIQ")
    
    # FTQ (Fetch Target Queue) size
    ftq_size = Param.Unsigned(16, "Maximum size of the FTQ")
    
    # Prefetch buffer size
    prefetch_buffer_size = Param.Unsigned(32, "Maximum size of the prefetch buffer")
    
    # Number of prefetch requests to issue per cycle
    prefetch_degree = Param.Unsigned(4, "Number of prefetch requests to issue per cycle")
    
    # Prefetch distance in blocks
    prefetch_distance = Param.Unsigned(2, "Prefetch distance in blocks")