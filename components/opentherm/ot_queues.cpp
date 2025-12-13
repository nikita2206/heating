/*
 * OpenTherm Queue Implementation (C++)
 *
 * Most functionality is in the header (template classes).
 * This file provides explicit instantiations if needed.
 */

#include "ot_queues.hpp"

namespace ot {

// Explicit template instantiation for Frame queue
template class Queue<Frame>;

} // namespace ot
