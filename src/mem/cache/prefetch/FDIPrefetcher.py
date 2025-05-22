# Copyright (c) 2024 All rights reserved
#
# The license below extends only to copyright in the software and shall
# not be construed as granting a license to any other intellectual
# property including but not limited to intellectual property relating
# to a hardware implementation of the functionality of the software
# licensed hereunder.  You may use the software subject to the license
# terms below provided that you ensure that this notice is replicated
# unmodified and in its entirety in all distributions of the software,
# modified or unmodified, in source code or in binary form.

from m5.params import *
from m5.proxy import *

from m5.objects.QueuedPrefetcher import QueuedPrefetcher

class FDIPrefetcher(QueuedPrefetcher):
    type = 'FDIPrefetcher'
    cxx_class = 'gem5::prefetch::FDIP'
    cxx_header = 'mem/cache/prefetch/fdip.hh'

    # Number of cache lines to prefetch
    degree = Param.Unsigned(2, "Number of cache lines to prefetch")
    
    # Whether to use TAGE branch predictor
    use_tage = Param.Bool(True, "Whether to use TAGE branch predictor")
    
    # Thread ID to use for branch prediction (single thread support)
    thread_id = Param.Unsigned(0, "Thread ID to use for branch prediction")
    
    # Cache line size in bytes
    line_size = Param.Unsigned(64, "Cache line size in bytes")
    
    # Maximum number of instructions to look ahead for prefetching
    max_lookahead = Param.Unsigned(16, "Maximum number of instructions to look ahead")
    
    # Cleanup interval for the prefetched addresses map
    cleanup_interval = Param.Tick(1000000, "Cleanup interval for the prefetched addresses map")
    
    # Minimum confidence threshold for branch predictions (0-100)
    confidence_threshold = Param.Unsigned(50, "Minimum confidence threshold for branch predictions (0-100)")
    
    # Whether to create a dedicated branch predictor for FDIP
    create_dedicated_predictor = Param.Bool(False, "Whether to create a dedicated branch predictor for FDIP")
    
    # TAGE branch predictor parameters (used if create_dedicated_predictor is True and use_tage is True)
    tage_params = Param.TAGE(NULL, "TAGE branch predictor parameters")
    
    # Tournament branch predictor parameters (used if create_dedicated_predictor is True and use_tage is False)
    tournament_params = Param.BranchPredictor(NULL, "Tournament branch predictor parameters")
    
    # Maximum size of the PIQ (Program Information Queue)
    piq_size = Param.Unsigned(32, "Maximum size of the PIQ (Program Information Queue)")
    
    # Maximum size of the FTQ (Fetch Target Queue)
    ftq_size = Param.Unsigned(16, "Maximum size of the FTQ (Fetch Target Queue)")
    
    # Maximum size of the Prefetch Buffer
    prefetch_buffer_size = Param.Unsigned(16, "Maximum size of the Prefetch Buffer")
    
    # Maximum number of branch streams to track
    max_streams = Param.Unsigned(16, "Maximum number of branch streams to track")
    
    # How far ahead the branch predictor should work compared to the prefetcher
    branch_predictor_lookahead = Param.Unsigned(32, "How far ahead the branch predictor should work compared to the prefetcher")