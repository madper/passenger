/*
 *  Phusion Passenger - https://www.phusionpassenger.com/
 *  Copyright (c) 2011-2016 Phusion Holding B.V.
 *
 *  "Passenger", "Phusion Passenger" and "Union Station" are registered
 *  trademarks of Phusion Holding B.V.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 *  THE SOFTWARE.
 */
#ifndef _PASSENGER_SPAWNING_KIT_CONTEXT_H_
#define _PASSENGER_SPAWNING_KIT_CONTEXT_H_

#include <boost/function.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/make_shared.hpp>
#include <string>
#include <algorithm>
#include <cstddef>

#include <ResourceLocator.h>
#include <RandomGenerator.h>
#include <Exceptions.h>
#include <Utils/JsonUtils.h>
#include <Utils/VariantMap.h>
#include <Core/UnionStation/Context.h>

namespace Passenger {
	namespace ApplicationPool2 {
		class Options;
	}
}

namespace Passenger {
namespace SpawningKit {

using namespace std;


class HandshakePrepare;
typedef boost::function<void (const char *data, unsigned int size)> OutputHandler;
typedef Passenger::ApplicationPool2::Options AppPoolOptions;


class Context {
private:
	friend class HandshakePrepare;


	mutable boost::mutex syncher;


	/****** Context-global configuration ******/

	// Used by DummySpawner and SpawnerFactory.
	//unsigned int concurrency;
	//unsigned int spawnerCreationSleepTime;
	//unsigned int spawnTime;

	// Other.
	unsigned int minPortRange, maxPortRange;


	/****** Working state ******/

	unsigned int nextPort;


	void finalizeConfig() {
		maxPortRange = std::max(minPortRange, maxPortRange);
		nextPort = std::max(std::min(nextPort, maxPortRange), minPortRange);
	}

public:
	/****** Dependencies ******/

	ResourceLocator *resourceLocator;
	RandomGeneratorPtr randomGenerator;
	string integrationMode;
	string instanceDir;
	//OutputHandler outputHandler;
	//UnionStation::ContextPtr unionStationContext;


	Context()
		: //concurrency(1),
		  //spawnerCreationSleepTime(0),
		  //spawnTime(0),
		  minPortRange(5000),
		  maxPortRange(65535),

		  nextPort(0),

		  resourceLocator(NULL)
		{ }

	void loadConfigFromJson(const Json::Value &doc, const string &prefix = string()) {
		TRACE_POINT();
		boost::lock_guard<boost::mutex> l(syncher);

		getJsonUintField(doc, prefix + "min_port_range", &minPortRange);
		getJsonUintField(doc, prefix + "max_port_range", &maxPortRange);

		finalizeConfig();
	}

	Json::Value getConfigAsJson(const string &prefix = string()) const {
		boost::lock_guard<boost::mutex> l(syncher);
		Json::Value doc;

		doc[prefix + "min_port_range"] = minPortRange;
		doc[prefix + "max_port_range"] = maxPortRange;

		return doc;
	}

	void finalize() {
		TRACE_POINT();
		if (resourceLocator == NULL) {
			throw RuntimeException("ResourceLocator not initialized");
		}
		if (randomGenerator == NULL) {
			randomGenerator = boost::make_shared<RandomGenerator>();
		}
		if (integrationMode.empty()) {
			throw RuntimeException("integrationMode not set");
		}
		finalizeConfig();
	}
};

typedef boost::shared_ptr<Context> ConfigPtr;


} // namespace SpawningKit
} // namespace Passenger

#endif /* _PASSENGER_SPAWNING_KIT_CONTEXT_H_ */
