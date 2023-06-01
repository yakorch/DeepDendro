//
// Created by Matthew Prytula on 20.05.2023.
//

#ifndef DEEPDENDRO_PIPELINEMODEL_H
#define DEEPDENDRO_PIPELINEMODEL_H

#include <tbb/tbb.h>
#include <algorithm>
#include <iostream>
#include "FlowLayer.h"
#include "FlowOutputLayer.h"
#include "SourceNode/MicrobatchSourceBody.h"
#include "MNISTProcess.h"


class PipelineModel {
    int microbatch_size;
    std::vector<std::shared_ptr<FlowLayer>> layers;
    std::shared_ptr<FlowOutputLayer> outputLayer;
    std::string pathToData;
    tbb::flow::graph g;
    std::mutex mtx4;
    std::mutex mtx5;
    std::mutex mtx6;
    tbb::concurrent_vector<std::mutex> mutexes;
    std::vector<size_t> microBatchCounters;
    std::vector<tbb::flow::function_node<MatrixXd, MatrixXd>> forwardNodes;
    std::vector<tbb::flow::function_node<MatrixXd, MatrixXd>> backwardNodes;
    std::vector<tbb::flow::function_node<bool, bool>> weightUpdates;
    double learning_rate;

    MatrixXd forward_propagate(MatrixXd& microbatch, int layerIndex) {
        return layers[layerIndex]->forward_prop(microbatch);
    }

    // Backward propagation through a layer
    MatrixXd backward_propagate(MatrixXd& gradient, int layerIndex) {
        return layers[layerIndex]->back_prop(gradient);
    }
public:
    PipelineModel(int micro_batch_size=8, double lr=0.005, const std::string& path = "../MNIST_ORG"): microbatch_size(micro_batch_size), learning_rate(lr), pathToData(path){
        outputLayer = std::make_shared<FlowOutputLayer>(10, Shape(8, 8), find_activation_func(softmax), 8);
    };

    void addLayer(int size, const Shape &shape, activation activationFunc, int updateAfter) {
        // Add new FlowLayer to vector of layers
        layers.push_back(std::make_shared<FlowLayer>(size, shape, find_activation_func(activationFunc), updateAfter));
    }

    void runConfPipeline() {

        MNISTProcess mnistProcessTrain = MNISTProcess();
        TrainingSet data = mnistProcessTrain.getTrainingData(pathToData);
        tbb::concurrent_queue<std::pair<MatrixXd, MatrixXd>> queue;
        // TODO: minibatch size have to be a parameter
        // TODO: divide into microbatches initially
        mnistProcessTrain.enqueueMiniBatchesFromMemory(microbatch_size, queue, data.trainData, data.trainLabels);

        MicrobatchSourceBody body(queue, microbatch_size);

        tbb::flow::input_node<MatrixXd> input(g,
                                              [this, &body](tbb::flow_control &fc) -> MatrixXd {
                                                  std::pair<MatrixXd, MatrixXd> microbatch;
                                                  if (body(microbatch)) {
                                                      if (microbatch.first.cols() == 1 && microbatch.first.rows() == 1 && microbatch.first(0, 0) == -1.0 &&
                                                          microbatch.second.cols() == 1 && microbatch.second.rows() == 1 && microbatch.second(0, 0) == -1.0) {
                                                          fc.stop();
                                                      }else {
                                                          this->outputLayer->set_labels(microbatch.second);
                                                      }
                                                      return microbatch.first;
                                                  }
                                              });



        (layers.size()+1, 0);
        mutexes = tbb::concurrent_vector<std::mutex>(layers.size()+1);
        microBatchCounters = std::vector<size_t>(layers.size()+1, 0);

        for (int i = 0; i < layers.size(); ++i) {
            forwardNodes.emplace_back(g, tbb::flow::unlimited, [this, i](MatrixXd m) -> MatrixXd {
                if (m.cols() == 1 && m.rows() == 1 && m(0, 0) == -1.0) {
                    return m; // Return immediately poison pill
                }
                return forward_propagate(m, i);
            });

            weightUpdates.emplace_back(g, tbb::flow::unlimited, [this, i](bool m) -> bool {
                layers[i]->update_weights(learning_rate, i);
                return true;
            });

            backwardNodes.emplace_back(g, tbb::flow::unlimited, [this, i](MatrixXd m) -> MatrixXd {
                std::lock_guard<std::mutex> lock(mutexes[i]);
                if (m.cols() == 1 && m.rows() == 1 && m(0, 0) == -1.0) {
                    return m; // Return immediately poison pill
                }
                MatrixXd grad = backward_propagate(m, i);
                microBatchCounters[i]++;
                // TODO: magic number remove
                if (microBatchCounters[i] == 8) {
                    microBatchCounters[i] = 0;
                    weightUpdates[i].try_put(true);
                }
                return grad;
            });
        }
        forwardNodes.emplace_back(g, tbb::flow::unlimited, [this](MatrixXd m) -> MatrixXd {
            if (m.cols() == 1 && m.rows() == 1 && m(0, 0) == -1.0) {
                return m; // Return immediately poison pill
            }
            return outputLayer->forward_prop(m);
        });

        weightUpdates.emplace_back(g, tbb::flow::unlimited, [this](bool m) -> bool {
            outputLayer->update_weights(learning_rate, 3);
            return true;
        });

        backwardNodes.emplace_back(g, tbb::flow::unlimited, [this](MatrixXd m) -> MatrixXd {
            int i = mutexes.size()-1;
            std::lock_guard<std::mutex> lock(mutexes[i]);
            if (m.cols() == 1 && m.rows() == 1 && m(0, 0) == -1.0) {
                return m; // Return immediately poison pill
            }
            MatrixXd grad = outputLayer->calc_first_back_prop(m);
            microBatchCounters[i]++;
            // TODO: magic number remove
            if (microBatchCounters[i] == 8) {
                microBatchCounters[i] = 0;
                weightUpdates[i].try_put(true);
            }
            return grad;
        });

        make_edge(input, forwardNodes[0]);
        make_edge(forwardNodes[forwardNodes.size()-1], backwardNodes[backwardNodes.size()-1]);
        for(int i = 0; i < layers.size(); ++i) {
            make_edge(forwardNodes[i], forwardNodes[i+1]);
            make_edge(backwardNodes[i+1], backwardNodes[i]);
        }

        for (int i = 0; i < 10; ++i) {
            std::cout << "Epoch: " << i << "\n";
            input.activate();
            g.wait_for_all();
            g.reset();
            mnistProcessTrain.enqueueMiniBatchesFromMemory(microbatch_size, queue, data.trainData, data.trainLabels);  // Use in-memory data
        }


    }


    void run_pipeline(tbb::concurrent_queue<std::pair<MatrixXd, MatrixXd>> &queue) {
        size_t microBatchCounter1 = 0;
        size_t microBatchCounter2 = 0;
        size_t microBatchCounter3 = 0;

        FlowLayer flowLayer1(16, {784, 8}, find_activation_func(relu), 8);

        FlowLayer flowLayer2(8, {16, 8}, find_activation_func(relu), 8);

        tbb::flow::function_node<bool, bool> weight_update1( g, tbb::flow::unlimited, [&flowLayer1, this](bool m) -> bool {
//             std::cout << "weight_update1 \n";
            flowLayer1.update_weights(learning_rate, 1);
            return true;
        } );

        tbb::flow::function_node<bool, bool> weight_update2( g, tbb::flow::unlimited, [&flowLayer2, this](bool m) -> bool {
//             std::cout << "weight_update2 \n";
            flowLayer2.update_weights(learning_rate, 2);
            return true;
        } );

        tbb::flow::function_node<bool, bool> weight_update3( g, tbb::flow::unlimited, [this](bool m) -> bool {
//             std::cout << "weight_update3 \n";
            outputLayer->update_weights(learning_rate, 3);
            return true;
        } );


        MicrobatchSourceBody body(queue, microbatch_size);

        tbb::flow::input_node<MatrixXd> input(g,
            [this, &body](tbb::flow_control &fc) -> MatrixXd {
                std::pair<MatrixXd, MatrixXd> microbatch;
                if (body(microbatch)) {
                    if (microbatch.first.cols() == 1 && microbatch.first.rows() == 1 && microbatch.first(0, 0) == -1.0 &&
                            microbatch.second.cols() == 1 && microbatch.second.rows() == 1 && microbatch.second(0, 0) == -1.0) {
                        fc.stop();
                    }else {
                        this->outputLayer->set_labels(microbatch.second);
                    }
                    return microbatch.first;
                }
            });


        tbb::flow::function_node<MatrixXd, MatrixXd> func1( g, tbb::flow::unlimited, [this, &flowLayer1]( MatrixXd m ) -> MatrixXd {
            if (m.cols() == 1 && m.rows() == 1 && m(0, 0) == -1.0) {
                return m; // Return immediately poison pill
            }

//            std::cout << "func1 receiving a_value from input\n";
            return flowLayer1.forward_prop(m, true);
        } );



        tbb::flow::function_node<MatrixXd, MatrixXd> func2( g, tbb::flow::unlimited, [this, &flowLayer2]( MatrixXd m ) -> MatrixXd {
            if (m.cols() == 1 && m.rows() == 1 && m(0, 0) == -1.0) {
                return m; // Return immediately poison pill
            }
//            std::cout << "func2 receiving a_value from func1\n";
            return flowLayer2.forward_prop(m);

        } );

        tbb::flow::function_node<MatrixXd, MatrixXd> func3( g, tbb::flow::unlimited, [this]( MatrixXd m ) -> MatrixXd {
            if (m.cols() == 1 && m.rows() == 1 && m(0, 0) == -1.0) {
                return m; // Return immediately poison pill
            }
//            std::cout << "func3 receiving a_value from func2\n";
            return this->outputLayer->forward_prop(m);
        } );

        tbb::flow::function_node<MatrixXd, MatrixXd> back_func3( g, tbb::flow::unlimited, [this, &microBatchCounter3, &weight_update3]( MatrixXd m ) -> MatrixXd {
            std::lock_guard<std::mutex> lock(mtx4);
            if (m.cols() == 1 && m.rows() == 1 && m(0, 0) == -1.0) {
                return m; // Return immediately poison pill
            }
//            std::cout << "back_prop3 receiving a_value from func3\n";
            MatrixXd grad = this->outputLayer->calc_first_back_prop(m);
            microBatchCounter3++;
            if (microBatchCounter3 == 8) {
//                std::cout << "func1 sending true to weight_update1\n";
                microBatchCounter3 = 0;
                weight_update3.try_put(true);
            }
            return grad;
        } );

        tbb::flow::function_node<MatrixXd, MatrixXd> back_func2( g, tbb::flow::unlimited, [this, &flowLayer2, &microBatchCounter2, &weight_update2]( MatrixXd m ) -> MatrixXd {
            std::lock_guard<std::mutex> lock(mtx5);
            if (m.cols() == 1 && m.rows() == 1 && m(0, 0) == -1.0) {
                return m; // Return immediately poison pill
            }
//            std::cout << "back_prop2 receiving a_value from back_prop3\n";
            MatrixXd grad = flowLayer2.back_prop(m);
            microBatchCounter2++;
            if (microBatchCounter2 == 8) {
//                std::cout << "func1 sending true to weight_update1\n";
                microBatchCounter2 = 0;
                weight_update2.try_put(true);
            }
            return grad;
        } );

        tbb::flow::function_node<MatrixXd, MatrixXd > back_func1( g, tbb::flow::unlimited, [this, &flowLayer1, &microBatchCounter1, &weight_update1]( MatrixXd m ) -> MatrixXd {
            std::lock_guard<std::mutex> lock(mtx6);
            if (m.cols() == 1 && m.rows() == 1 && m(0, 0) == -1.0) {
                return m; // Return immediately poison pill
            }
            MatrixXd grad = flowLayer1.back_prop(m);
            microBatchCounter1++;
            if (microBatchCounter1 == 8) {
//                std::cout << "func1 sending true to weight_update1\n";
                microBatchCounter1 = 0;
                weight_update1.try_put(true);
            }
//            std::cout << "back_prop1 receiving a_value from back_prop2\n";
            return grad;
        } );



        make_edge( input, func1 );
        make_edge( func1, func2 );
        make_edge( func2, func3 );
        make_edge( func3, back_func3 );
        make_edge( back_func3, back_func2 );
        make_edge( back_func2, back_func1 );
//        make_edge( weight_update1, weight_update2 );
//        make_edge( weight_update2, weight_update3 );

        MNISTProcess mnistProcessTrain = MNISTProcess();
        std::string pathToMNIST = "../MNIST_ORG";
        TrainingSet data = mnistProcessTrain.getTrainingData(pathToMNIST);

        for (int i = 0; i < 10; ++i) {
            std::cout << "Epoch: " << i << "\n";
            input.activate();
            g.wait_for_all();
            g.reset();
            mnistProcessTrain.enqueueMiniBatchesFromMemory(32, queue, data.trainData, data.trainLabels);  // Use in-memory data
        }
//        input.activate();
//        g.wait_for_all();
    }
};


#endif //DEEPDENDRO_PIPELINEMODEL_H
