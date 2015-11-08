#include "clstm_compute.h"

// FIXME: factor out nonlinearities

namespace ocropus {

#ifndef DEVICE
typedef Eigen::DefaultDevice Device;
#else
typedef DEVICE Device;
#endif

Eigen::DefaultDevice default_device;

inline Eigen::array<Eigen::IndexPair<int>, 1> axispairs(int i, int j) {
  Eigen::array<Eigen::IndexPair<int>, 1> result = {Eigen::IndexPair<int>(i, j)};
  return result;
}

inline Eigen::array<ptrdiff_t, 1> indexes(int i) { 
  return Eigen::array<ptrdiff_t, 1>({i}); 
}

inline Eigen::array<ptrdiff_t, 2> indexes(int i, int j) {
  return Eigen::array<ptrdiff_t, 2>({i, j});
}

typedef Float (*FloatFun)(Float);

struct Nonlinearity {
  FloatFun nonlin;
  FloatFun yderiv;
  FloatFun xderiv;
};

Nonlinearity nonlinearities[] = {
  {
    [](Float x) { return x; },
    [](Float y) { return Float(1); },
    [](Float x) { return Float(1); },
  },
  {
    [](Float x) { return sigmoid(x); },
    [](Float y) { return y * (1-y); },
    [](Float x) { Float y = sigmoid(x); return y * (1-y); }
  },
  {
    [](Float x) { return tanh(x); },
    [](Float y) { return 1 - y*y; },
    [](Float x) { Float y = tanh(x); return 1 - y*y; }
  },
  {
    [](Float x) { return x<0?0:x; },
    [](Float y) { return Float(y<=0?0:1); },
    [](Float x) { return Float(x<=0?0:1); }
  }
};

void forward_identity(Device *dev, Batch &y, Batch &x) {
  y.v().device(*dev) = x.v();
}
void forward_sigmoid(Device *dev, Batch &y, Batch &x) {
  y.v().device(*dev) = x.v().sigmoid();
}
void forward_tanh(Device *dev, Batch &y, Batch &x) {
  y.v().device(*dev) = x.v().tanh();
}
void forward_relu(Device *dev, Batch &y, Batch &x) {
  y.v().device(*dev) = x.v().unaryExpr([](Float x) { return x<=0?0:x; });
}
void forward_nonlin(Device *dev, Batch &y, Batch &x, int nl) {
  switch(nl) {
  case LIN: forward_identity(dev, y, x); break;
  case SIG: forward_sigmoid(dev, y, x); break;
  case TANH: forward_tanh(dev, y, x); break;
  case RELU: forward_relu(dev, y, x); break;
  default: abort();
  }
}

void backward_identity(Device *dev, Batch &y) {
  y.d().device(*dev) = y.d();
}
void backward_sigmoid(Device *dev, Batch &y) {
  y.d().device(*dev) = y.v() * (-y.v()+Float(1)) * y.d();
}
void backward_tanh(Device *dev, Batch &y) {
  y.d().device(*dev) = (-y.v()*y.v() + Float(1)) * y.d();
}
void backward_relu(Device *dev, Batch &y) {
  y.d().device(*dev) = y.d() * 
    y.v().unaryExpr([](Float y) { return Float(y<=0?0:1); });
}
void backward_nonlin(Device *dev, Batch &y, int nl) {
  switch(nl) {
  case LIN: backward_identity(dev, y); break;
  case SIG: backward_sigmoid(dev, y); break;
  case TANH: backward_tanh(dev, y); break;
  case RELU: backward_relu(dev, y); break;
  default: abort();
  }
}

void backward_identity(Device *dev, Batch &y, Batch &x) {
  x.d().device(*dev) += y.d();
}
void backward_sigmoid(Device *dev, Batch &y, Batch &x) {
  x.d().device(*dev) += y.v() * (-y.v()+Float(1)) * y.d();
}
void backward_tanh(Device *dev, Batch &y, Batch &x) {
  x.d().device(*dev) += (-y.v()*y.v() + Float(1)) * y.d();
}
void backward_relu(Device *dev, Batch &y, Batch &x) {
  x.d().device(*dev) += y.d() * 
    y.v().unaryExpr([](Float y) { return Float(y<=0?0:1); });
}
void backward_nonlin(Device *dev, Batch &y, Batch &x, int nl) {
  switch(nl) {
  case LIN: backward_identity(dev, y, x); break;
  case SIG: backward_sigmoid(dev, y, x); break;
  case TANH: backward_tanh(dev, y, x); break;
  case RELU: backward_relu(dev, y, x); break;
  default: abort();
  }
}

// full layers with constant offset

void forward_lin1(Device *dev, Batch &y, Params &W1, Batch &x) {
  int n = W1.v.dimension(0), m = W1.v.dimension(1);
  int bs = x.v.dimension(1);
  assert(y.rows() == n);
  assert(y.cols() == x.cols());
  assert(x.rows() == m-1);
  y.v().device(*dev) =
      (W1.v().slice(indexes(0, 1), indexes(n, m - 1)).contract(x.v(), axispairs(1, 0)) +
       W1.v().chip(0, 1).reshape(indexes(n, 1)).broadcast(indexes(1, bs)));
}
void backward_lin1(Device *dev, Batch &y, Params &W1, Batch &x) {
  int n = W1.v.dimension(0), m = W1.v.dimension(1);
  x.d().device(*dev) += W1.v().slice(indexes(0, 1), indexes(n, m - 1)).contract(y.d(), axispairs(0, 0));
  W1.d().slice(indexes(0, 1), indexes(n, m - 1)).device(*dev) += y.d().contract(x.v(), axispairs(1, 1));
  W1.d().chip(0, 1).device(*dev) += y.d().sum(indexes(1));
}

// full layers with nonlinearities

#if 1
void forward_full1(Device *dev, Batch &y, Params &W1, Batch &x, Nonlin nl) {
  forward_lin1(dev, y, W1, x);
  forward_nonlin(dev, y, y, nl);
}
#else
void forward_full1(Device *dev,Batch &y, Params &W1, Batch &x, Nonlin nl) {
  Float (*f)(Float) = nonlinearities[nl].nonlin;
  int n = W1.v.dimension(0), m = W1.v.dimension(1);
  int bs = x.v.dimension(1);
  assert(y.rows() == n);
  assert(y.cols() == x.cols());
  assert(x.rows() == m-1);
  y.v().device(*dev) =
      (W1.v().slice(indexes(0, 1), indexes(n, m - 1)).contract(x.v(), axispairs(1, 0)) +
       W1.v().chip(0, 1).reshape(indexes(n, 1)).broadcast(indexes(1, bs))).unaryExpr(f);
}
#endif


#if 1
void backward_full1(Device *dev, Batch &y, Params &W1, Batch &x, Nonlin nl) {
  backward_nonlin(dev, y, nl);
  backward_lin1(dev, y, W1, x);
}
#else
void backward_full1(Device *dev, Batch &y, Params &W1, Batch &x, Nonlin nl) {
  Float (*g)(Float) = nonlinearities[nl].yderiv;
  int n = W1.v.dimension(0), m = W1.v.dimension(1);
  EigenTensor2 temp = y.v().unaryExpr(g) * y.d();
  x.d().device(*dev) += W1.v().slice(indexes(0, 1), indexes(n, m - 1)).contract(temp, axispairs(0, 0));
  W1.d().slice(indexes(0, 1), indexes(n, m - 1)).device(*dev) += temp.contract(x.v(), axispairs(1, 1));
  W1.d().chip(0, 1).device(*dev) += temp.sum(indexes(1));
}
#endif

// softmax

void forward_softmax(Device *dev, Batch &z, Params &W1, Batch &x) {
  Float (*f)(Float) = limexp;
  int n = W1.v.dimension(0);
  int m = W1.v.dimension(1);
  int bs = z.v.dimension(1);
  assert(n == z.v.dimension(0));
  assert(n >= 2);
  z.v().device(*dev) = (W1.v()
             .slice(indexes(0, 1), indexes(n, m - 1))
             .contract(x.v(), axispairs(1, 0)) +
         W1.v().chip(0, 1).reshape(indexes(n, 1)).broadcast(indexes(1, bs)))
            .unaryExpr(f);
  EigenTensor1 sums = z.v().sum(indexes(0));
  assert(sums.dimension(0)==bs);
  z.v().device(*dev) = z.v() / sums.reshape(indexes(1,bs)).broadcast(indexes(n,1));;
}
void backward_softmax(Device *dev, Batch &z, Params &W1, Batch &x) {
  int n = W1.v.dimension(0), m = W1.v.dimension(1);
  int bs = z.v.dimension(1);
  x.d().device(*dev) = W1.v().slice(indexes(0, 1), indexes(n, m - 1)).contract(z.d(), axispairs(0, 0));
  W1.d().slice(indexes(0, 1), indexes(n, m - 1)).device(*dev) += z.d().contract(x.v(), axispairs(1, 1));
  W1.d().chip(0, 1).device(*dev) += z.d().sum(indexes(1));
}

// stacking

void forward_stack(Device *dev, Batch &z, Batch &x, Batch &y) {
  int nx = x.v.dimension(0), ny = y.v.dimension(0);
  int bs = x.v.dimension(1);
  assert(z.rows() == x.rows() + y.rows());
  assert(z.cols() == x.cols() && z.cols() == y.cols());
  z.v().slice(indexes(0, 0), indexes(nx, bs)).device(*dev) = x.v();
  z.v().slice(indexes(nx, 0), indexes(ny, bs)).device(*dev) = y.v();
}
void backward_stack(Device *dev, Batch &z, Batch &x, Batch &y) {
  int nx = x.v.dimension(0), ny = y.v.dimension(0);
  int bs = x.v.dimension(1);
  x.d().device(*dev) += z.d().slice(indexes(0, 0), indexes(nx, bs));
  y.d().device(*dev) += z.d().slice(indexes(nx, 0), indexes(ny, bs));
}

// stacking with delay

void forward_stack_delay(Device *dev, Batch &z, Batch &x, Sequence &y, int last) {
  int nx = x.v.dimension(0), ny = y[0].v.dimension(0);
  int bs = x.v.dimension(1);
  assert(z.rows() == x.rows() + y.rows());
  assert(z.cols() == x.cols() && z.cols() == y.cols());
  z.v().slice(indexes(0, 0), indexes(nx, bs)).device(*dev) = x.v();
  if (last >= 0)
    z.v().slice(indexes(nx, 0), indexes(ny, bs)).device(*dev) = y[last].v();
  else
    z.v().slice(indexes(nx, 0), indexes(ny, bs)).setZero();
}
void backward_stack_delay(Device *dev, Batch &z, Batch &x, Sequence &y, int last) {
  int nx = x.v.dimension(0), ny = y[0].v.dimension(0);
  int bs = x.v.dimension(1);
  x.d().device(*dev) += z.d().slice(indexes(0, 0), indexes(nx, bs));
  if (last >= 0) y[last].d().device(*dev) += z.d().slice(indexes(nx, 0), indexes(ny, bs));
}

// reverse sequences

void forward_reverse(Device *dev, Sequence &y, Sequence &x) {
  int N = x.size();
  for (int i = 0; i < N; i++) y[N - i - 1] = x[i];
}
void backward_reverse(Device *dev, Sequence &y, Sequence &x) {
  int N = x.size();
  for (int i = 0; i < N; i++) x[N - i - 1].d().device(*dev) += y[i].d();
}

// combine the delayed gated state with the gated input

void forward_statemem(Device *dev, Batch &state, Batch &ci, Batch &gi, Sequence &states,
                      int last, Batch &gf) {
  state.v().device(*dev) = ci.v() * gi.v();
  if (last >= 0) state.v().device(*dev) += gf.v() * states[last].v();
}
void backward_statemem(Device *dev, Batch &state, Batch &ci, Batch &gi, Sequence &states,
                       int last, Batch &gf) {
  if (last >= 0) states[last].d().device(*dev) += state.d() * gf.v();
  if (last >= 0) gf.d().device(*dev) += state.d() * states[last].v();
  gi.d().device(*dev) += state.d() * ci.v();
  ci.d().device(*dev) += state.d() * gi.v();
}

// linear gated output

void forward_gate(Device *dev, Batch &out, Batch &nlstate, Batch &go) {
  out.v().device(*dev) = nlstate.v() * go.v();
}
void backward_gate(Device *dev, Batch &out, Batch &nlstate, Batch &go) {
  go.d().device(*dev) += nlstate.v() * out.d();
  nlstate.d().device(*dev) += go.v() * out.d();
}

// nonlinear gated output

#if 1
void forward_nonlingate(Device *dev, Batch &out, Batch &state, Batch &go, int nl) {
  Batch temp;
  temp.resize(out.rows(), out.cols());
  forward_nonlin(dev, temp, state, nl);
  forward_gate(dev, out, temp, go);
}
#else
void forward_nonlingate(Device *dev, Batch &out, Batch &state, Batch &go, Nonlin nl) {
  Float (*f)(Float) = nonlinearities[nl].nonlin;
  out.v().device(*dev) = state.v().unaryExpr(f) * go.v();
}
#endif

#if 1
void backward_nonlingate(Device *dev, Batch &out, Batch &state, Batch &go, int nl) {
  Batch temp;
  temp.resize(out.rows(), out.cols());
  forward_nonlin(dev, temp, state, nl);
  backward_gate(dev, out, temp, go);
  backward_nonlin(dev, temp, state, nl);
}
#else
void backward_nonlingate(Device *dev, Batch &out, Batch &state, Batch &go, Nonlin nl) {
  Float (*f)(Float) = nonlinearities[nl].nonlin;
  Float (*g)(Float) = nonlinearities[nl].xderiv;
  go.d().device(*dev) += state.v().unaryExpr(f) * out.d();
  state.d().device(*dev) += state.v().unaryExpr(g) * go.v() * out.d();
}
#endif

}

