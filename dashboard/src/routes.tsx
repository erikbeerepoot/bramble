import { createBrowserRouter, Navigate } from 'react-router-dom';
import App from './App';
import NodeList from './components/NodeList';
import NodeDetailPage from './components/NodeDetailPage';
import Settings from './components/Settings';

export const router = createBrowserRouter([
  {
    path: '/',
    element: <App />,
    children: [
      {
        index: true,
        element: <Navigate to="/nodes" replace />,
      },
      {
        path: 'nodes',
        element: <NodeList />,
      },
      {
        path: 'nodes/:address',
        element: <NodeDetailPage />,
      },
      {
        path: 'settings',
        element: <Settings />,
      },
    ],
  },
]);
