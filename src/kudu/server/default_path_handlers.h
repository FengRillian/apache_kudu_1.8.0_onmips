// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#ifndef KUDU_SERVER_DEFAULT_PATH_HANDLERS_H
#define KUDU_SERVER_DEFAULT_PATH_HANDLERS_H

namespace kudu {

class MetricRegistry;
class Webserver;

// Adds a set of default path handlers to the webserver to display
// logs and configuration flags.
void AddDefaultPathHandlers(Webserver* webserver);

// Adds an endpoint to get metrics in JSON format.
void RegisterMetricsJsonHandler(Webserver* webserver, const MetricRegistry* const metrics);

} // namespace kudu

#endif // KUDU_SERVER_DEFAULT_PATH_HANDLERS_H
