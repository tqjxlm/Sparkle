#ifndef VIEW_H_
#define VIEW_H_

struct View
{
    matrix ViewProjectionMatrix;
    matrix ViewMatrix;
    matrix ProjectionMatrix;
    matrix InvViewMatrix;
    matrix InvProjectionMatrix;
    float near;
    float far;
};

#endif // VIEW_H_
