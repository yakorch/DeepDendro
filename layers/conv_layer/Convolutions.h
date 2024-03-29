//
// Created by Yaroslav Korch on 05.05.2023.
//

#ifndef DEEPDENDRO_CONVOLUTIONS_H
#define DEEPDENDRO_CONVOLUTIONS_H

#include "ConvLayerBase.h"

class Convolutional3D : public ConvLayer<3> {
public:
    explicit Convolutional3D(size_t n_filters, Shape filters_shape, activation activ_func, Shape input_shape);
};


class Convolutional2D : public ConvLayer<2> {
public:
    explicit Convolutional2D(size_t n_filters, Shape filters_shape, activation activ_func, Shape input_shape);
};



#endif //DEEPDENDRO_CONVOLUTIONS_H
